"""Tests for esl_client.py against a small fake AP (python3 -m unittest)."""

import datetime
import io
import json
import os
import tempfile
import threading
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from http.server import BaseHTTPRequestHandler, HTTPServer

import esl_client

TOKEN = "secret"


class FakeAp(BaseHTTPRequestHandler):
    tags = {}
    calls = []
    wifi = {}
    time = None
    sched = []
    config = {"poll_s": 0, "builtin_poll_s": 0, "webhook": None}

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
        if c.get("at"):
            FakeAp.sched.append(dict(c, mac=mac))
            return {"mac": mac, "scheduled": True, "at": c["at"]}
        cid = abs(hash(json.dumps(c, sort_keys=True))) % 2**32 or 1
        old = self.tags.get(mac, {})
        changed = old.get("content_id") != cid
        self.tags[mac] = {"mac": mac, "content_id": cid, "shown_content_id": cid, "pending": False,
                          "model": "2.9", "seen_s_ago": 3, "battery_mv": 2950, "rssi": -60}
        return {"mac": mac, "content_id": cid, "changed": changed, "pending": False}

    def do_GET(self):
        if not self._auth():
            return
        if self.path == "/api/wifi":
            self._send(200, self.wifi)
        elif self.path == "/api/time":
            self._send(200, {"time": FakeAp.time, "source": "api" if FakeAp.time else None})
        elif self.path == "/api/schedule":
            self._send(200, {"time": FakeAp.time, "max_scheduled": 256, "scheduled": FakeAp.sched})
        elif self.path == "/api/config":
            self._send(200, self.config)
        elif self.path.startswith("/api/health"):
            self.calls.append((self.path, None))
            problems = [{"mac": m, "problems": ["low_battery"], "seen_s_ago": t["seen_s_ago"],
                         "battery_mv": t["battery_mv"]} for m, t in self.tags.items() if t["battery_mv"] < 2500]
            self._send(200, {"problems": problems, "ok": not problems, "tags": len(self.tags),
                             "with_problems": len(problems), "silent": 0, "unseen": 0, "draw_failed": 0,
                             "low_battery": len(problems), "layout_failed": 0, "content_error": 0})
        elif self.path == "/api/tags":
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
        if self.path == "/api/time":
            FakeAp.time = body["time"]
            self._send(200, {"time": FakeAp.time, "source": "api"})
        if self.path == "/api/config":
            if "poll_s" in body:
                FakeAp.config["poll_s"] = body["poll_s"] or 0
            if "webhook" in body:
                FakeAp.config["webhook"] = body["webhook"] or None
            self._send(200, FakeAp.config)
        elif self.path == "/api/wifi":
            FakeAp.wifi = {"mode": "sta", "ssid": body["ssid"] or None, "connected": False,
                           "setup_ssid": None, "networks": []}
            self._send(200, FakeAp.wifi)
        elif self.path == "/api/tags":
            if len(body["tags"]) > esl_client.BATCH_MAX:
                self._send(413, {"error": "too many"})
                return
            bad = [i for i, r in enumerate(body["tags"]) if "BAD" in r.get("fields", [])]
            if bad:
                self._send(422, {"error": "layout \"BAD\" is not in any layout file", "index": bad[0]})
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
        FakeAp.sched = []
        FakeAp.time = None

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

    def test_health_and_config(self):
        rc, out = self.run_cli("health")
        self.assertEqual((rc, out), (0, "ok: 0 of 0 tags\n"))
        self.run_cli("price", "00000123456789A8", "Milk", "24.90")
        FakeAp.tags["00000123456789A8"]["battery_mv"] = 2380
        rc, out = self.run_cli("health", "--silent-s", "600")
        self.assertEqual(rc, 1)
        self.assertIn("PROBLEMS: 1 of 1 tags (low_battery 1)", out)
        self.assertIn("00000123456789A8  low_battery", out)
        self.assertIn("2380 mV", out)
        self.assertEqual(FakeAp.calls[-1][0], "/api/health?silent_s=600")

        rc, out = self.run_cli("config")
        self.assertEqual((rc, out), (0, "poll_s 0 (menuconfig 0), webhook off\n"))
        rc, out = self.run_cli("config", "--webhook", "http://10.0.0.5:8080/esl", "--poll-s", "120")
        self.assertEqual(rc, 0)
        self.assertEqual(FakeAp.calls[-1], ("/api/config", {"poll_s": 120, "webhook": "http://10.0.0.5:8080/esl"}))
        self.assertIn("poll_s 120 (menuconfig 0), webhook http://10.0.0.5:8080/esl", out)
        rc, out = self.run_cli("config", "--webhook", "", "--poll-s", "null")
        self.assertEqual(FakeAp.calls[-1], ("/api/config", {"poll_s": None, "webhook": None}))
        self.assertIn("webhook off", out)

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

    def write(self, text, encoding="utf-8"):
        f = tempfile.NamedTemporaryFile("wb", suffix=".csv", delete=False)
        f.write(text.encode(encoding))
        f.close()
        self.addCleanup(os.unlink, f.name)
        return f.name

    def run_err(self, *args):
        err = io.StringIO()
        with redirect_stderr(err):
            rc, out = self.run_cli(*args)
        return rc, out, err.getvalue()

    def test_sync_excel_semicolons(self):
        # Excel on a Norwegian Windows: ; between columns, Windows-1252, decimal comma
        path = self.write("MAC;Navn;Pris;Layout\r\n0000000000000001;Brød;24,90;price\r\n", "cp1252")
        rc, out = self.run_cli("sync", path)
        self.assertEqual(rc, 0, out)
        self.assertEqual(FakeAp.calls[0][1]["tags"], [{"mac": "0000000000000001", "layout": "price",
                                                       "fields": ["Brød", "24,90"]}])

    def test_sync_columns_by_name(self):
        path = self.write("name,mac,price,frame\nMelk,0000000000000002,19.90,1\nOst,0000000000000003,,\n")
        rc, out = self.run_cli("sync", path, "--layout", "price")
        self.assertEqual(rc, 0, out)
        self.assertEqual(FakeAp.calls[0][1]["tags"], [
            {"mac": "0000000000000002", "layout": "price", "fields": ["Melk", "19.90"], "frame": True},
            {"mac": "0000000000000003", "layout": "price", "fields": ["Ost"]}])

    def test_sync_items_to_tags(self):
        tags = self.write("mac,item\n0000000000000001,100\n0000000000000002,100\n0000000000000003,200\n"
                          "0000000000000004,300\n")
        prices = self.write("item,layout,name,price\n100,price,Melk,19.90\n200,price,Ost,89.00\n")
        rc, out = self.run_cli("sync", prices, "--tags", tags)
        self.assertEqual(rc, 0, out)
        sent = {r["mac"]: r["fields"] for r in FakeAp.calls[0][1]["tags"]}
        self.assertEqual(sent, {"0000000000000001": ["Melk", "19.90"], "0000000000000002": ["Melk", "19.90"],
                                "0000000000000003": ["Ost", "89.00"]})
        self.assertIn("item '300'", out)

    def test_sync_errors_send_nothing(self):
        path = self.write("mac,layout,name\n0000000000000001,price,A\n12,price,B\n"
                          "0000000000000001,price,C\n0000000000000004,,D\n")
        rc, out, err = self.run_err("sync", path)
        self.assertEqual(rc, 1)
        self.assertEqual(FakeAp.calls, [])
        self.assertIn(f"{path}:3: not a 16-digit hex MAC", err)
        self.assertIn(f"{path}:4: 0000000000000001 already has content from line 2", err)
        self.assertIn(f"{path}:5: no layout", err)

    def test_sync_ap_error_names_the_line(self):
        lines = "".join(f"{i:016X},price,Vare {i}\n" for i in range(1, 61))
        path = self.write("mac,layout,name\n" + lines.replace("Vare 55", "BAD"))
        rc, out, err = self.run_err("sync", path)
        self.assertEqual(rc, 2)
        self.assertIn(f"{path}:56: layout", err)
        self.assertIn("the 50 tags before", err)

    def test_sync_dry_run(self):
        path = self.write("mac,layout,name,price\n0000000000000001,price,Melk,19.90\n")
        rc, out = self.run_cli("sync", path, "--dry-run")
        self.assertEqual(rc, 0)
        self.assertEqual(FakeAp.calls, [])
        self.assertIn("0000000000000001  price       Melk | 19.90", out)
        self.assertIn("1 tags would be sent", out)

    def test_schedule_sets_the_clock_first(self):
        rc, out = self.run_cli("price", "0000000000000001", "Melk", "22.90", "--at", "2030-01-02 07:00")
        self.assertEqual(rc, 0, out)
        self.assertIn("set it from this computer", out)
        self.assertEqual(FakeAp.calls[0], ("/api/time", {"time": FakeAp.time}))
        at = int(datetime.datetime(2030, 1, 2, 7, 0).timestamp())
        self.assertEqual(FakeAp.calls[1][1]["at"], at)
        self.assertIn("scheduled for 2030-01-02 07:00", out)
        # the AP knows the time now, so it is not set again
        self.run_cli("price", "0000000000000001", "Melk", "21.90", "--at", "2030-01-03")
        self.assertEqual([p for p, _ in FakeAp.calls].count("/api/time"), 1)
        rc, out = self.run_cli("schedule")
        self.assertIn("2030-01-02 07:00  0000000000000001", out)
        self.assertIn("2 of 256 scheduled", out)

    def test_sync_at(self):
        path = self.write("mac,layout,name,price,at\n"
                          "0000000000000001,price,Melk,19.90,\n"
                          "0000000000000001,price,Melk,14.90,2030-01-02 07:00\n"
                          "0000000000000002,price,Ost,89.00,\n")
        rc, out = self.run_cli("sync", path, "--at", "1893456000")
        self.assertEqual(rc, 0, out)
        sent = [(r["mac"], r.get("at")) for r in FakeAp.calls[-1][1]["tags"]]
        at = int(datetime.datetime(2030, 1, 2, 7, 0).timestamp())
        self.assertEqual(sent, [("0000000000000001", 1893456000), ("0000000000000001", at),
                                ("0000000000000002", 1893456000)])
        self.assertIn("3 scheduled", out)
        bad = self.write("mac,layout,name,at\n0000000000000001,price,A,2030-01-02\n"
                         "0000000000000001,price,B,2030-01-02\n0000000000000002,price,C,neste uke\n")
        rc, out, err = self.run_err("sync", bad)
        self.assertEqual(rc, 1)
        self.assertIn(f"{bad}:3: 0000000000000001 already has content from line 2 for the same time", err)
        self.assertIn(f"{bad}:4: not a time: 'neste uke'", err)

    def test_wifi(self):
        FakeAp.wifi = {"mode": "sta+setup", "ssid": "Home", "connected": False, "setup_ssid": "openesl-1A2B",
                       "networks": [{"ssid": "Home", "rssi": -50}]}
        rc, out = self.run_cli("wifi")
        self.assertEqual(rc, 0)
        self.assertIn("trying to join 'Home'", out)
        self.assertIn("openesl-1A2B", out)
        self.assertIn(" -50 dBm  Home", out)
        rc, out = self.run_cli("wifi", "Butikken", "--password", "hemmelig123")
        self.assertEqual(rc, 0)
        self.assertEqual(FakeAp.calls[-1], ("/api/wifi", {"ssid": "Butikken", "password": "hemmelig123"}))
        self.assertIn("trying to join 'Butikken'", out)

    def test_no_answer(self):
        cli = esl_client.Client("http://127.0.0.1:1", TOKEN, timeout=0.2, retries=2)
        with self.assertRaises(ConnectionError):
            cli.tags()


class FakeSerialAp(threading.Thread):
    """Answers "@ESL" frames on a pty like ap/main/serial_api.c, with log lines
    around each reply."""

    def __init__(self, fd, replies, piece=0):
        super().__init__(daemon=True)
        self.fd = fd
        self.replies = replies      # list of (status, body bytes)
        self.requests = []
        self.piece = piece          # > 0: send the body in pieces of this size, "+" after the length


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
            if self.piece:
                while len(body) > self.piece:
                    os.write(self.fd, b"@ESL " + seq + b" %d %d+ " % (status, self.piece) + body[:self.piece] + b"\n")
                    os.write(self.fd, b"I (150) http: GET /api/tags -> 200\n")
                    body = body[self.piece:]
            os.write(self.fd, b"@ESL " + seq + b" %d %d " % (status, len(body)) + body + b"\n")
            os.write(self.fd, b"W (200) wifi: disconnected\n")
        time.sleep(5)               # keep the master open: closing it hangs up the port


class SerialClientTest(unittest.TestCase):
    def _client(self, replies, piece=0):
        master, slave = os.openpty()
        ap = FakeSerialAp(master, replies, piece)
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

    def test_reply_in_pieces(self):
        tags = [{"mac": "%016X" % (0x2000 + i), "content_id": i, "pending": i % 2 == 0} for i in range(40)]
        body = json.dumps({"ap": {"fw": "0.1.0"}, "tags": tags}).encode()
        cli, _ = self._client([(200, body)], piece=700)
        self.assertEqual(cli.tags()["tags"], tags)

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


class BridgeTest(unittest.TestCase):
    """serve: the bridge the API tab of the web page uses to try the API."""

    class Recorder:
        url = "fake"

        def __init__(self):
            self.calls = []

        def send(self, method, path, data=None, auth=None):
            self.calls.append((method, path, data, auth))
            return 201, b'{"ok":true}'

    def setUp(self):
        from http.server import ThreadingHTTPServer
        self.cli = self.Recorder()
        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), esl_client.make_bridge(self.cli))
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.addCleanup(self.httpd.server_close)
        self.addCleanup(self.httpd.shutdown)
        self.base = f"http://127.0.0.1:{self.httpd.server_address[1]}"

    def _req(self, method, path, origin=None, data=None, headers=None):
        import urllib.error
        import urllib.request
        h = dict(headers or {})
        if origin:
            h["Origin"] = origin
        req = urllib.request.Request(self.base + path, data=data, method=method, headers=h)
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, r.headers, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.headers, e.read()

    def test_forwards_from_the_project_page(self):
        status, headers, body = self._req("PUT", "/api/tags/00000123456789A8", "https://mathiasbk.github.io",
                                          b'{"layout":"text","fields":["a"]}', {"Authorization": "Bearer x"})
        self.assertEqual((status, body), (201, b'{"ok":true}'))
        self.assertEqual(headers["Access-Control-Allow-Origin"], "https://mathiasbk.github.io")
        self.assertEqual(self.cli.calls, [("PUT", "/api/tags/00000123456789A8",
                                           b'{"layout":"text","fields":["a"]}', "Bearer x")])

    def test_preflight(self):
        status, headers, _ = self._req("OPTIONS", "/api/tags", "http://localhost:5173")
        self.assertEqual(status, 204)
        self.assertEqual(headers["Access-Control-Allow-Origin"], "http://localhost:5173")
        self.assertIn("Authorization", headers["Access-Control-Allow-Headers"])
        self.assertEqual(headers["Access-Control-Allow-Private-Network"], "true")

    def test_foreign_page_is_refused(self):
        self.assertEqual(self._req("OPTIONS", "/api/tags", "https://evil.example")[0], 403)
        self.assertEqual(self._req("PUT", "/api/tags", "https://evil.example", b"{}")[0], 403)
        self.assertEqual(self.cli.calls, [])

    def test_only_the_api(self):
        self.assertEqual(self._req("GET", "/")[0], 404)
        self.assertEqual(self._req("GET", "/api/tags")[0], 201)        # curl: no Origin


if __name__ == "__main__":
    unittest.main()
