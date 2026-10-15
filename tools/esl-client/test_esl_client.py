"""Tests for esl_client.py against a small fake AP (python3 -m unittest)."""

import io
import json
import os
import tempfile
import threading
import time
import unittest
from contextlib import redirect_stdout
from http.server import BaseHTTPRequestHandler, HTTPServer

import esl_client

TOKEN = "secret"


class FakeAp(BaseHTTPRequestHandler):
    tags = {}
    calls = []

    def log_message(self, *a):
        pass

    def _send(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        n = int(self.headers.get("Content-Length", 0))
        return json.loads(self.rfile.read(n)) if n else None

    def _auth(self):
        if self.headers.get("Authorization") != f"Bearer {TOKEN}":
            self._send(401, {"error": "unauthorized"})
            return False
        return True

    def _set(self, mac, c):
        cid = abs(hash(json.dumps(c, sort_keys=True))) % 2**32 or 1
        old = self.tags.get(mac, {})
        changed = old.get("content_id") != cid
        self.tags[mac] = {"mac": mac, "content_id": cid, "shown_content_id": cid, "pending": False,
                          "model": "2.9", "seen_s_ago": 3, "battery_mv": 2950, "rssi": -60}
        return {"mac": mac, "content_id": cid, "changed": changed, "pending": False}

    def do_GET(self):
        if not self._auth():
            return
        if self.path == "/api/tags":
            self._send(200, {"ap": {"fw": "0.1.0", "uptime_s": 10, "channel": 25, "max_tags": 64},
                             "tags": list(self.tags.values())})
        elif self.path.startswith("/api/tags/"):
            t = self.tags.get(self.path.rsplit("/", 1)[1])
            self._send(200 if t else 404, t or {"error": "not found"})
        else:
            self._send(404, {})

    def do_PUT(self):
        if not self._auth():
            return
        body = self._body()
        self.calls.append((self.path, body))
        if self.path == "/api/tags":
            if len(body["tags"]) > esl_client.BATCH_MAX:
                self._send(413, {"error": "too many"})
                return
            self._send(200, {"tags": [self._set(r["mac"], {k: v for k, v in r.items() if k != "mac"})
                                      for r in body["tags"]]})
        else:
            if len(" ".join(body["fields"]).encode("latin-1", "replace")) > 92:
                self._send(422, {"error": "text too long"})
                return
            self._send(200, self._set(self.path.rsplit("/", 1)[1], body))


class ClientTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv = HTTPServer(("127.0.0.1", 0), FakeAp)
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()
        cls.url = f"http://127.0.0.1:{cls.srv.server_port}"

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()

    def setUp(self):
        FakeAp.tags.clear()
        FakeAp.calls.clear()

    def run_cli(self, *args):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = esl_client.main(["--url", self.url, "--token", TOKEN, *args])
        return rc, out.getvalue()

    def test_price_and_status(self):
        rc, out = self.run_cli("price", "00:00:01:23:45:67:89:a8", "Milk 1 L", "24.90", "x", "--highlight")
        self.assertEqual(rc, 0)
        self.assertIn("changed", out)
        path, body = FakeAp.calls[0]
        self.assertEqual(path, "/api/tags/00000123456789A8")
        self.assertEqual(body, {"layout": "price", "fields": ["Milk 1 L", "24.90", "x"], "price_highlight": True})
        rc, out = self.run_cli("status")
        self.assertIn("00000123456789A8", out)
        self.assertIn("shown", out)

    def test_same_content_is_unchanged(self):
        self.run_cli("text", "00000123456789A8", "Hello", "--frame")
        rc, out = self.run_cli("text", "00000123456789A8", "Hello", "--frame")
        self.assertIn("unchanged", out)
        self.assertEqual(FakeAp.calls[0][1], {"layout": "text", "fields": ["Hello"], "frame": True})

    def test_wait_returns_when_shown(self):
        rc, _ = self.run_cli("price", "00000123456789A8", "A", "1", "--wait", "--wait-s", "1")
        self.assertEqual(rc, 0)

    def test_text_too_long_is_error(self):
        rc, _ = self.run_cli("text", "00000123456789A8", "x" * 100)
        self.assertEqual(rc, 2)

    def test_bad_token(self):
        rc = esl_client.main(["--url", self.url, "--token", "wrong", "status"])
        self.assertEqual(rc, 2)

    def test_bad_mac_rejected(self):
        with self.assertRaises(SystemExit):
            esl_client.main(["--url", self.url, "--token", TOKEN, "show", "1234"])

    def test_sync_chunks_of_50(self):
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False, encoding="utf-8") as f:
            f.write("mac,layout,highlight,frame,name,price,info\n")
            for i in range(120):
                f.write(f"{i:016X},price,{i % 2},,Vare {i},{i}.90,kr/kg\n")
        try:
            rc, out = self.run_cli("sync", f.name)
        finally:
            os.unlink(f.name)
        self.assertEqual(rc, 0)
        self.assertIn("120 tags sent", out)
        sizes = [len(b["tags"]) for p, b in FakeAp.calls]
        self.assertEqual(sizes, [50, 50, 20])
        self.assertTrue(FakeAp.calls[0][1]["tags"][1]["price_highlight"])
        self.assertNotIn("price_highlight", FakeAp.calls[0][1]["tags"][0])

    def test_no_answer(self):
        cli = esl_client.Client("http://127.0.0.1:1", TOKEN, timeout=0.2, retries=2)
        with self.assertRaises(ConnectionError):
            cli.tags()


class FakeSerialAp(threading.Thread):
    """Answers "@ESL" frames on a pty like ap/main/serial_api.c, with log lines
    around each reply."""

    def __init__(self, fd, replies):
        super().__init__(daemon=True)
        self.fd = fd
        self.replies = replies      # list of (status, body bytes)
        self.requests = []

    def run(self):
        f = os.fdopen(self.fd, "rb", buffering=0)
        for status, body in self.replies:
            line = b""
            while not line.endswith(b"\n"):
                line += f.read(1)
            _, seq, method, path, n = line.split()
            data = f.read(int(n)) if int(n) else b""
            self.requests.append((method.decode(), path.decode(), data))
            os.write(self.fd, b"I (100) radio: rx 12 B\n@ESL 99 200 2 {}\n")
            os.write(self.fd, b"@ESL " + seq + b" %d %d " % (status, len(body)) + body + b"\n")
            os.write(self.fd, b"W (200) wifi: disconnected\n")
        time.sleep(5)               # keep the master open: closing it hangs up the port


class SerialClientTest(unittest.TestCase):
    def _client(self, replies):
        master, slave = os.openpty()
        ap = FakeSerialAp(master, replies)
        ap.start()
        cli = esl_client.SerialClient(os.ttyname(slave), timeout=2)
        cli.fd = cli._open()    # before the last slave fd closes, or the pty hangs up
        os.close(slave)
        self.addCleanup(os.close, cli.fd)
        return cli, ap

    def test_request_and_reply(self):
        cli, ap = self._client([(200, b'{"mac":"00000123456789A8","content_id":7,\n"pending":true}')])
        r = cli.set_tag("00000123456789A8", {"layout": "price", "fields": ["Milk", "24.90"]})
        self.assertEqual(r["content_id"], 7)
        method, path, data = ap.requests[0]
        self.assertEqual((method, path), ("PUT", "/api/tags/00000123456789A8"))
        self.assertEqual(json.loads(data)["fields"], ["Milk", "24.90"])

    def test_error_status(self):
        cli, _ = self._client([(422, b'{"error":"text too long"}')])
        with self.assertRaises(esl_client.ApiError) as e:
            cli.tags()
        self.assertEqual(e.exception.status, 422)

    def test_no_answer(self):
        cli, _ = self._client([])
        cli.timeout = 0.3
        with self.assertRaises(ConnectionError):
            cli.tags()


if __name__ == "__main__":
    unittest.main()
