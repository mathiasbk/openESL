#!/usr/bin/env python3
"""Writes the backup file of a release for the compatibility test (#307).

  python3 tools/esl-client/compat/make_backup.py v0.10.0

Takes esl_client.py and its tests from that release (git show, nothing in the working
tree changes), fills the fake AP of those tests with tags, settings, a layout file, a
picture and a scheduled entry, and saves what that release's "backup" writes as
tools/esl-client/compat/<release>-backup.json. test_esl_client.py then checks that
today's "restore" takes it. Run it once per release, in the VERSION PR, and commit the
file; never write one for a release again.
"""
import importlib
import json
import os
import subprocess
import sys
import tempfile
import threading
from http.server import HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
FUTURE = 4102444800  # 2100-01-01: still ahead whenever the file is restored


def main(argv):
    if len(argv) != 2 or not argv[1].startswith("v"):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    release = argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        for name in ("esl_client.py", "test_esl_client.py"):
            src = subprocess.run(["git", "-C", ROOT, "show", f"{release}:tools/esl-client/{name}"], check=True,
                                 capture_output=True).stdout
            with open(os.path.join(tmp, name), "wb") as f:
                f.write(src)
        sys.path.insert(0, tmp)
        client = importlib.import_module("esl_client")
        tests = importlib.import_module("test_esl_client")
        fake = tests.FakeAp
        tests.ClientTest.setUp(None)  # the fake's empty state, as before every test of that release
        srv = HTTPServer(("127.0.0.1", 0), fake)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        try:
            cli = client.Client(f"http://127.0.0.1:{srv.server_port}", tests.TOKEN)
            fake.layouts["2.9"] = {"format": "esl-layouts", "version": 1, "model": "2.9", "layouts": [
                {"name": "shelf", "fields": ["a"], "elements": [{"type": "text", "field": "a", "x": 0, "y": 0,
                                                                 "w": 10, "h": 20}]}]}
            fake.config.update({"poll_s": 120, "webhook": "http://192.0.2.5:8080/esl"})
            cli.set_tag("00000123456789A8", {"layout": "price", "fields": ["Milk 1 L", "24.90"],
                                             "price_highlight": True})
            cli.set_tag("00000123456789A9", {"layout": "text", "fields": ["Open", "9-21"], "frame": True})
            cli.set_tag("00000123456789AA", {"layout": "shelf", "fields": ["Coffee"]})
            fake.tag_cfg["00000123456789A9"] = {"poll_s": 300}
            fake.tags["00000123456789A9"]["config"] = fake._cfg("00000123456789A9")
            fake.tags["00000123456789B1"] = dict(fake.tags["00000123456789A8"], mac="00000123456789B1")
            cli.set_image("00000123456789B1", bytes(range(256)) * 18 + bytes(128))
            fake.wifi = {"mode": "sta", "ssid": "Shop", "connected": True, "hostname": "shop"}
            fake.sched = [{"mac": "00000123456789A8", "at": FUTURE, "layout": "price", "fields": ["Milk 1 L", "19.90"]}]
            b = client.backup(cli, now=1793000000)  # a fixed time, so the file is the same every run
        finally:
            srv.shutdown()
    out = os.path.join(HERE, f"{release}-backup.json")
    with open(out, "w", encoding="utf-8") as f:
        f.write(json.dumps(b, indent=1, ensure_ascii=False) + "\n")
    print(f"{os.path.relpath(out, ROOT)}: {len(b['tags'])} tags, {len(b['layouts'])} layout files, "
          f"{len(b['scheduled'])} scheduled")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
