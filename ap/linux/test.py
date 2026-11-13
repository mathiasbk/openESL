#!/usr/bin/env python3
"""Tests openesl-ap (ap/linux) as a program, over HTTP, with the simulated radio.

    python3 test.py build/host/openesl-ap

Starts the binary with --radio sim on a free local port and a fresh data directory, and
checks that the API answers with and without the token, that both tag models get content
drawn and sent block by block until they report it, that a restart keeps the content, the
AP id and the token, and that the dashboard is served. Exits 1 on the first failure.
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
        check(not os.path.exists(os.path.join(data, "content", "1")) or
              ap.json("GET", f"/api/tags/{TAGS[1]}").get("content_id") in (0, None),
              "a deleted tag's content is gone")
        check(ap.stop() == 0, "SIGTERM stops it cleanly again")
    finally:
        log = os.path.join(tmp, "ap.log")
        if os.path.exists(log) and os.environ.get("OPENESL_AP_TEST_LOG"):
            print(open(log).read())
        shutil.rmtree(tmp, ignore_errors=True)
    print("all openesl-ap tests passed")


if __name__ == "__main__":
    main()
