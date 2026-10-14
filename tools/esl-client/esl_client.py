#!/usr/bin/env python3
"""Example client for the OpenESL AP HTTP API (docs/protokoll.md part 2).

Standard library only. URL and token come from --url/--token or the environment
variables OPENESL_URL (default http://openesl.local) and OPENESL_TOKEN.

  esl_client.py status
  esl_client.py price 00000123456789A8 "Milk 1 L" 24.90 "NOK 24.90/L" --highlight --wait
  esl_client.py text  00000123456789A8 "Opening hours" "Mon-Fri 9-21" "Sat 9-18" --frame
  esl_client.py sync prices.csv          # full send, chunks of 50
  esl_client.py delete 00000123456789A8
  esl_client.py layouts
  esl_client.py layout-put 2.9 layout.json

CSV for sync: mac,layout,highlight,frame,field1,field2,... with a header row.
highlight and frame are 1/0, true/false or empty.
"""

import argparse
import csv
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

BATCH_MAX = 50          # max tags per PUT /api/tags (protokoll.md part 2)
MAC_RE = re.compile(r"^[0-9A-F]{16}$")


class ApiError(Exception):
    def __init__(self, status, body):
        super().__init__(f"HTTP {status}: {body}")
        self.status = status
        self.body = body


class Client:
    def __init__(self, url, token, timeout=10.0, retries=3):
        self.url = url.rstrip("/")
        self.token = token
        self.timeout = timeout
        self.retries = retries

    def _request(self, method, path, body=None, raw=False):
        data = None
        headers = {"Authorization": f"Bearer {self.token}"}
        if body is not None:
            data = body if isinstance(body, bytes) else json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        last = None
        for attempt in range(self.retries):
            req = urllib.request.Request(self.url + path, data=data, method=method, headers=headers)
            try:
                with urllib.request.urlopen(req, timeout=self.timeout) as r:
                    payload = r.read()
                    return payload if raw else (json.loads(payload) if payload else None)
            except urllib.error.HTTPError as e:
                # The AP answered: retrying will not help
                raise ApiError(e.code, e.read().decode("utf-8", "replace")) from None
            except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
                last = e
                time.sleep(0.5 * (2 ** attempt))
        raise ConnectionError(f"no answer from {self.url}: {last}")

    def tags(self):
        return self._request("GET", "/api/tags")

    def tag(self, mac):
        return self._request("GET", f"/api/tags/{mac}")

    def set_tag(self, mac, content):
        return self._request("PUT", f"/api/tags/{mac}", content)

    def set_many(self, rows):
        """rows: list of dicts with "mac" plus content. Sent in chunks of BATCH_MAX."""
        out = []
        for i in range(0, len(rows), BATCH_MAX):
            reply = self._request("PUT", "/api/tags", {"tags": rows[i:i + BATCH_MAX]})
            out.extend(reply.get("tags", []) if isinstance(reply, dict) else reply)
        return out

    def delete_tag(self, mac):
        return self._request("DELETE", f"/api/tags/{mac}")

    def layouts(self):
        return self._request("GET", "/api/layouts")

    def put_layout(self, model, raw):
        return self._request("PUT", f"/api/layouts/{model}", raw)

    def wait_shown(self, mac, content_id, timeout_s=120, poll_s=5):
        """Wait until the tag reports content_id as shown (requirement G1: 2 min)."""
        end = time.monotonic() + timeout_s
        while True:
            t = self.tag(mac)
            if t.get("shown_content_id") == content_id:
                return True
            if time.monotonic() >= end:
                return False
            time.sleep(poll_s)


def norm_mac(s):
    mac = s.replace(":", "").replace("-", "").upper()
    if not MAC_RE.match(mac):
        raise argparse.ArgumentTypeError(f"not a 16-digit hex MAC: {s}")
    return mac


def as_bool(s):
    return str(s).strip().lower() in ("1", "true", "yes", "ja", "x")


def content(layout, fields, highlight=False, frame=False):
    c = {"layout": layout, "fields": list(fields)}
    if highlight:
        c["price_highlight"] = True
    if frame:
        c["frame"] = True
    return c


def read_csv(path):
    rows = []
    with open(path, newline="", encoding="utf-8-sig") as f:
        r = csv.reader(f)
        header = next(r, None)
        if not header or [h.strip().lower() for h in header[:4]] != ["mac", "layout", "highlight", "frame"]:
            raise SystemExit("CSV header must start with mac,layout,highlight,frame")
        for n, row in enumerate(r, start=2):
            if not row or not row[0].strip():
                continue
            try:
                mac = norm_mac(row[0].strip())
            except argparse.ArgumentTypeError as e:
                raise SystemExit(f"{path}:{n}: {e}")
            c = content(row[1].strip(), row[4:], as_bool(row[2]), as_bool(row[3]))
            c["mac"] = mac
            rows.append(c)
    return rows


def fmt_ago(s):
    if s is None:
        return "-"
    if s < 120:
        return f"{s} s"
    if s < 7200:
        return f"{s // 60} min"
    return f"{s // 3600} h"


def print_status(st):
    ap = st.get("ap", {})
    print(f"AP fw {ap.get('fw')}  uptime {fmt_ago(ap.get('uptime_s'))}  channel {ap.get('channel')}  "
          f"tags {len(st.get('tags', []))}/{ap.get('max_tags')}")
    print(f"{'MAC':16}  {'model':5}  {'seen':>7}  {'batt':>6}  {'rssi':>4}  state")
    for t in st.get("tags", []):
        if t.get("content_id", 0) == 0:
            state = "new, no content"
        elif t.get("pending"):
            state = "pending"
        else:
            state = "shown"
        if t.get("draw_failed"):
            state += ", DRAW FAILED"
        batt = t.get("battery_mv")
        print(f"{t['mac']:16}  {t.get('model') or '-':5}  {fmt_ago(t.get('seen_s_ago')):>7}  "
              f"{(str(batt) + ' mV') if batt is not None else '-':>6}  "
              f"{t.get('rssi') if t.get('rssi') is not None else '-':>4}  {state}")


def set_one(cli, args, c):
    r = cli.set_tag(args.mac, c)
    print(f"{r['mac']}: content_id {r['content_id']}, "
          f"{'changed' if r.get('changed') else 'unchanged'}, {'pending' if r.get('pending') else 'shown'}")
    if args.wait and r.get("pending"):
        print("waiting for the tag to show it ...", flush=True)
        ok = cli.wait_shown(args.mac, r["content_id"], timeout_s=args.wait_s)
        print("shown" if ok else f"not shown after {args.wait_s} s")
        return 0 if ok else 1
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description="OpenESL AP example client")
    p.add_argument("--url", default=os.environ.get("OPENESL_URL", "http://openesl.local"))
    p.add_argument("--token", default=os.environ.get("OPENESL_TOKEN", ""))
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status", help="list all tags")
    s = sub.add_parser("show", help="one tag with its content")
    s.add_argument("mac", type=norm_mac)

    for name, help_ in (("price", "layout price: name, price, small text"),
                        ("text", "layout text: up to four lines")):
        s = sub.add_parser(name, help=help_)
        s.add_argument("mac", type=norm_mac)
        s.add_argument("fields", nargs="+")
        if name == "price":
            s.add_argument("--highlight", action="store_true", help="white price in a black box")
        s.add_argument("--frame", action="store_true", help="black frame")
        s.add_argument("--wait", action="store_true", help="wait until the tag shows it")
        s.add_argument("--wait-s", type=int, default=120)

    s = sub.add_parser("sync", help="full send from CSV (chunks of 50)")
    s.add_argument("csv")
    s = sub.add_parser("delete", help="remove a tag from the AP")
    s.add_argument("mac", type=norm_mac)
    sub.add_parser("layouts", help="list layout files per model")
    s = sub.add_parser("layout-put", help="upload a layout file for a model")
    s.add_argument("model", choices=["2.9", "1.54"])
    s.add_argument("file")

    args = p.parse_args(argv)
    if not args.token:
        p.error("no token: use --token or OPENESL_TOKEN")
    cli = Client(args.url, args.token)

    try:
        if args.cmd == "status":
            print_status(cli.tags())
        elif args.cmd == "show":
            print(json.dumps(cli.tag(args.mac), indent=2, ensure_ascii=False))
        elif args.cmd == "price":
            return set_one(cli, args, content("price", args.fields, args.highlight, args.frame))
        elif args.cmd == "text":
            return set_one(cli, args, content("text", args.fields, False, args.frame))
        elif args.cmd == "sync":
            rows = read_csv(args.csv)
            res = cli.set_many(rows)
            changed = sum(1 for r in res if r.get("changed"))
            print(f"{len(res)} tags sent, {changed} changed, "
                  f"{sum(1 for r in res if r.get('pending'))} pending")
        elif args.cmd == "delete":
            cli.delete_tag(args.mac)
            print(f"{args.mac} deleted")
        elif args.cmd == "layouts":
            print(json.dumps(cli.layouts(), indent=2))
        elif args.cmd == "layout-put":
            with open(args.file, "rb") as f:
                print(json.dumps(cli.put_layout(args.model, f.read())))
    except ApiError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    except ConnectionError as e:
        print(f"error: {e}", file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
