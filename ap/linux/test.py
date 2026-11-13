#!/usr/bin/env python3
"""Tests openesl-ap (ap/linux) as a program, over HTTP, with the simulated radio.

    python3 test.py build/host/openesl-ap

Starts the binary with --radio sim on a free local port and a fresh data directory, and
checks that the API answers with and without the token, that both tag models get content
drawn and sent block by block until they report it, that a restart keeps the content, the
AP id and the token, that an AP id file that cannot be read is not replaced, that the
dashboard is served, and the HTTP server's limits (connections, bad heads). Exits 1 on
the first failure.
"""
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

SIM = "sim:2,200"
TAGS = ("00000000FAC70001", "00000000FAC70002")  # 2.9" and 1.54"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Ap:
    def __init__(self, binary, data, token=None):
        self.port = free_port()
        args = [binary, "--data", data, "--listen", f"127.0.0.1:{self.port}", "--radio", SIM]
        if token:
            args += ["--token", token]
        self.log = open(os.path.join(os.path.dirname(data), "ap.log"), "ab")
        self.p = subprocess.Popen(args, stdout=self.log, stderr=self.log)
        self.token = token
        for _ in range(100):
            try:
                self.request("GET", "/dashboard", auth=False)
                return
            except (urllib.error.URLError, ConnectionError):
                time.sleep(0.05)
        raise SystemExit("openesl-ap did not start")

    def request(self, method, path, body=None, auth=True):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}", data=data, method=method)
        if auth and self.token:
            req.add_header("Authorization", f"Bearer {self.token}")
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def json(self, method, path, body=None):
        status, raw = self.request(method, path, body)
        if status >= 300:
            check(False, f"{method} {path} -> {status} {raw[:200]!r}")
        return json.loads(raw)

    def stop(self):
        self.p.send_signal(signal.SIGTERM)
        rc = self.p.wait(timeout=10)
        self.log.close()
        return rc


def raw_request(port, data):
    """Sends data as it is; the status of the answer, None when the server just closed"""
    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
        try:
            s.sendall(data)
            line = s.recv(64).split(b"\r\n")[0].split()
        except (ConnectionResetError, BrokenPipeError):
            return None
    return int(line[1]) if len(line) > 1 else None


def check(ok, what):
    if not ok:
        print(f"FAIL: {what}")
        sys.exit(1)
    print(f"ok   {what}")


def wait_for(ap, what, cond, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        tags = {t["mac"]: t for t in ap.json("GET", "/api/tags")["tags"]}
        if cond(tags):
            return tags
        time.sleep(0.1)
    check(False, what)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    binary = os.path.abspath(sys.argv[1])
    tmp = tempfile.mkdtemp(prefix="openesl-ap-test-")
    data = os.path.join(tmp, "data")
    try:
        # first start: the token is made and kept in the data directory
        ap = Ap(binary, data)
        token_file = os.path.join(data, "token")
        check(os.path.exists(token_file), "a token is made at the first start")
        ap.token = open(token_file).read().strip()
        check(ap.request("GET", "/api/tags", auth=False)[0] == 401, "the API wants the token")
        status, _ = ap.request("GET", "/", auth=False)
        check(status in (200, 302), "/ answers")
        check(ap.request("GET", "/dashboard", auth=False)[0] == 200, "the dashboard is served")

        tags = wait_for(ap, "both simulated tags poll", lambda t: all(m in t for m in TAGS))
        check(tags[TAGS[0]]["model"] == "2.9" and tags[TAGS[1]]["model"] == "1.54", "the models are read from POLL")

        ids = {}
        for mac, price in zip(TAGS, ("24.90", "9.90")):
            r = ap.json("PUT", f"/api/tags/{mac}", {"layout": "price", "fields": ["Milk 1 L", price, "NOK"]})
            check(r["changed"] and r["pending"], f"{mac}: content accepted")
            ids[mac] = r["content_id"]
        wait_for(ap, "both tags fetch their image and report it",
                 lambda t: all(t[m]["shown_content_id"] == ids[m] and not t[m]["pending"] for m in TAGS))
        ap_id = ap.json("GET", "/api/config").get("ap_id")
        check(isinstance(ap_id, str) and len(ap_id) == 8, f"the AP has an id ({ap_id})")
        check(ap.stop() == 0, "SIGTERM stops it cleanly")

        # restart on the same data: content, AP id and token kept
        ap = Ap(binary, data, token=None)
        ap.token = open(token_file).read().strip()
        tags = wait_for(ap, "the tags are known after a restart", lambda t: all(m in t for m in TAGS))
        check(all(tags[m]["content_id"] == ids[m] for m in TAGS), "the content survives a restart")
        check(ap.json("GET", "/api/config").get("ap_id") == ap_id, "the AP id survives a restart")
        wait_for(ap, "the restarted simulated tags get their image again",
                 lambda t: all(t[m]["shown_content_id"] == ids[m] for m in TAGS))
        r = ap.request("DELETE", f"/api/tags/{TAGS[1]}")
        check(r[0] < 300, "a tag can be deleted")
        status, raw = ap.request("GET", f"/api/tags/{TAGS[1]}")
        check(status == 404 or json.loads(raw).get("content_id") in (0, None), "a deleted tag's content is gone")

        # the HTTP server's limits
        check(raw_request(ap.port, b"PUT /api/tags/x HTTP/1.1\r\nContent-Length: 12abc\r\n\r\n") == 400,
              "a Content-Length that is not a number gets 400")
        check(raw_request(ap.port, b"PUT /api/tags/x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n") == 411,
              "a chunked body gets 411")
        check(raw_request(ap.port, b"GET /api/" + b"x" * 600 + b" HTTP/1.1\r\n\r\n") == 414, "a long URI gets 414")
        check(raw_request(ap.port, b"GET / HTTP/1.1\r\nX: " + b"x" * 9000 + b"\r\n\r\n") in (431, None),
              "a head over 8 KB is refused")
        idle = [socket.create_connection(("127.0.0.1", ap.port)) for _ in range(40)]
        status = raw_request(ap.port, b"GET /dashboard HTTP/1.1\r\n\r\n")
        for s_ in idle:
            s_.close()
        check(status == 503, "more than 32 connections at once get 503")
        time.sleep(0.3)
        check(ap.request("GET", "/dashboard", auth=False)[0] == 200, "it serves again once they close")
        check(ap.stop() == 0, "SIGTERM stops it cleanly again")

        # an AP id file that exists but cannot be read whole is not replaced: no id this start
        id_file = os.path.join(data, "ap_id")
        with open(id_file, "r+b") as f:
            f.truncate(3)
        ap = Ap(binary, data)
        ap.token = open(token_file).read().strip()
        check(ap.json("GET", "/api/config").get("ap_id") is None, "a cut ap_id file gives no AP id")
        check(os.path.getsize(id_file) == 3, "and is left as it is")
        check(ap.stop() == 0, "SIGTERM stops it cleanly a third time")
    finally:
        log = os.path.join(tmp, "ap.log")
        if os.path.exists(log) and os.environ.get("OPENESL_AP_TEST_LOG"):
            print(open(log).read())
        shutil.rmtree(tmp, ignore_errors=True)
    print("all openesl-ap tests passed")


if __name__ == "__main__":
    main()
