#!/usr/bin/env python3
"""Example client for the OpenESL AP HTTP API (docs/protokoll.md part 2).

Standard library only. URL and token come from --url/--token or the environment
variables OPENESL_URL (default http://openesl.local) and OPENESL_TOKEN.
With --serial PORT (or OPENESL_SERIAL) it talks to the AP over its USB port
instead (Linux/macOS); no token is needed there.

  esl_client.py status
  esl_client.py price 00000123456789A8 "Milk 1 L" 24.90 "NOK 24.90/L" --highlight --wait
  esl_client.py text  00000123456789A8 "Opening hours" "Mon-Fri 9-21" "Sat 9-18" --frame
  esl_client.py sync prices.csv          # full send, chunks of 50
  esl_client.py sync prices.csv --at "2026-10-19 07:00"   # the new prices from Monday 07:00
  esl_client.py schedule                 # what is scheduled
  esl_client.py delete 00000123456789A8
  esl_client.py layouts
  esl_client.py layout-put 2.9 layout.json
  esl_client.py ota build/esl_ap.bin    # new AP firmware; "ota" alone shows the slots
  esl_client.py tag-ota openesl-tag-1.54-v0.3.0-ota.bin   # new tag firmware, sent over the radio
  esl_client.py tag-ota                 # the tag firmware on the AP and how far the tags are
  esl_client.py log week.csv --every 60 # long-term test: one row per tag per minute
  esl_client.py report week.csv         # gaps, battery, problems and AP restarts from that log
  esl_client.py --serial /dev/ttyACM0 status
  esl_client.py --serial /dev/ttyACM0 serve   # for "Test request" in the API tab of the web page

CSV for sync: a header row names the columns: mac, layout, highlight and frame
(1/0, true/false or empty); every other column is a field, in column order.
With --tags tags.csv (mac,item) the file has an item column instead of mac.
"""

import argparse
import csv
import datetime
import io
import json
import os
import re
import select
import sys
import threading
import time
import urllib.error
import urllib.request

BATCH_MAX = 50          # max tags per PUT /api/tags (protokoll.md part 2)
OTA_PART = 16384        # bytes per POST /api/ota part on the serial port (ap/main/serial_api.c)
TAGFW_PART = 16384      # bytes per PUT /api/tag-firmware part (below the AP's 24 KB body limit)
TAGFW_MAGIC = b"OSFW"   # first bytes of a tag firmware file (tag/tools/build.py, shared/proto.h ESL_FWF_*)
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

    def send(self, method, path, data=None, auth=None, content_type="application/json"):
        """One request as it is: returns (status, body bytes). auth replaces the token."""
        headers = {"Authorization": auth or f"Bearer {self.token}"}
        if data is not None:
            headers["Content-Type"] = content_type
        last = None
        for attempt in range(self.retries):
            req = urllib.request.Request(self.url + path, data=data, method=method, headers=headers)
            try:
                with urllib.request.urlopen(req, timeout=self.timeout) as r:
                    return r.status, r.read()
            except urllib.error.HTTPError as e:
                # The AP answered: retrying will not help
                return e.code, e.read()
            except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
                last = e
                time.sleep(0.5 * (2 ** attempt))
        raise ConnectionError(f"no answer from {self.url}: {last}")

    def _request(self, method, path, body=None, raw=False):
        data = None
        if body is not None:
            data = body if isinstance(body, bytes) else json.dumps(body).encode("utf-8")
        status, payload = self.send(method, path, data)
        if status >= 400:
            raise ApiError(status, payload.decode("utf-8", "replace"))
        return payload if raw else (json.loads(payload) if payload else None)

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

    def tag_config(self, mac):
        return self._request("GET", f"/api/tags/{mac}/config")

    def set_tag_config(self, mac, cfg):
        """cfg: poll_s, max_poll_s, lost_ap_s; a key left out or None means the default."""
        return self._request("PUT", f"/api/tags/{mac}/config", cfg)

    def layouts(self):
        return self._request("GET", "/api/layouts")

    def layout(self, model):
        """The layout file as uploaded, or None when the model has none."""
        try:
            return self._request("GET", f"/api/layouts/{model}")
        except ApiError as e:
            if e.status == 404:
                return None
            raise

    def put_layout(self, model, raw):
        return self._request("PUT", f"/api/layouts/{model}", raw)

    def health(self, silent_s=None, battery_mv=None):
        q = [f"silent_s={silent_s}" if silent_s else "", f"battery_mv={battery_mv}" if battery_mv else ""]
        q = "&".join(x for x in q if x)
        return self._request("GET", "/api/health" + ("?" + q if q else ""))

    def config(self):
        return self._request("GET", "/api/config")

    def set_config(self, **kw):
        """poll_s=None resets the interval, webhook=None or "" turns the webhook off."""
        return self._request("PUT", "/api/config", kw)

    def wifi(self):
        return self._request("GET", "/api/wifi")

    def ota_info(self):
        return self._request("GET", "/api/ota")

    def ota(self, image, progress=None):
        """Sends a new AP firmware (build/esl_ap.bin) in one POST; the AP restarts
        into it a second after the reply. progress(sent, total) is called as it goes."""
        status, payload = self.send("POST", "/api/ota", image, content_type="application/octet-stream")
        if status >= 400:
            raise ApiError(status, payload.decode("utf-8", "replace"))
        if progress:
            progress(len(image), len(image))
        return json.loads(payload)

    def tag_firmware(self):
        return self._request("GET", "/api/tag-firmware")

    def put_tag_firmware(self, data, progress=None):
        """Uploads a tag firmware file (openesl-tag-<model>-<version>-ota.bin) in parts of TAGFW_PART bytes.
        Returns the AP's reply to the last part: the image it now offers."""
        total = len(data)
        reply = None
        for off in range(0, total, TAGFW_PART):
            part = data[off:off + TAGFW_PART]
            status, payload = self.send("PUT", f"/api/tag-firmware?offset={off}&total={total}", part,
                                        content_type="application/octet-stream")
            if status >= 400:
                raise ApiError(status, payload.decode("utf-8", "replace"))
            reply = json.loads(payload)
            if progress:
                progress(off + len(part), total)
        return reply

    def delete_tag_firmware(self, model):
        return self._request("DELETE", f"/api/tag-firmware/{model}")

    def set_wifi(self, ssid, password=""):
        return self._request("PUT", "/api/wifi", {"ssid": ssid, "password": password})

    def time(self):
        return self._request("GET", "/api/time")

    def set_time(self, unix_s):
        return self._request("PUT", "/api/time", {"time": int(unix_s)})

    def schedule(self):
        return self._request("GET", "/api/schedule")

    def clear_schedule(self, mac=None):
        return self._request("DELETE", f"/api/schedule/{mac}" if mac else "/api/schedule")

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


class SerialClient(Client):
    """The same API over the AP's USB port (ap/main/serial_api.c). Requests and
    replies are "@ESL" frames in the log stream; everything else is skipped."""

    def __init__(self, port, timeout=10.0):
        super().__init__("serial:" + port, "", timeout, retries=1)
        self.port = port
        self.seq = 0
        self.fd = None

    def _open(self):
        import termios
        fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY)
        a = termios.tcgetattr(fd)
        a[0] = 0                                          # iflag: raw
        a[1] = 0                                          # oflag: raw
        a[2] = (a[2] | termios.CLOCAL | termios.CREAD) & ~termios.HUPCL
        a[3] = 0                                          # lflag: no echo, not canonical
        a[6][termios.VMIN], a[6][termios.VTIME] = 1, 0
        termios.tcsetattr(fd, termios.TCSANOW, a)
        termios.tcflush(fd, termios.TCIFLUSH)
        return fd

    def ota(self, image, progress=None):
        """The serial driver drops bytes when its buffer is full, so the image
        goes in parts of OTA_PART bytes, each acknowledged before the next."""
        total = len(image)
        reply = None
        for off in range(0, total, OTA_PART):
            part = image[off:off + OTA_PART]
            status, payload = self.send("POST", f"/api/ota?offset={off}&total={total}", part)
            if status >= 400:
                raise ApiError(status, payload.decode("utf-8", "replace"))
            reply = json.loads(payload)
            if progress:
                progress(off + len(part), total)
        return reply

    def _read(self, n, end):
        out = b""
        while len(out) < n:
            left = end - time.monotonic()
            if left <= 0 or not select.select([self.fd], [], [], left)[0]:
                raise TimeoutError
            chunk = os.read(self.fd, n - len(out))
            if not chunk:
                raise ConnectionError("serial port closed")
            out += chunk
        return out

    def _readline(self, end):
        line = b""
        while not line.endswith(b"\n"):
            line += self._read(1, end)
        return line

    def send(self, method, path, data=None, auth=None, content_type=None):
        """As Client.send; no token is needed on the USB port, so auth is ignored, and
        there are no headers, so content_type is too."""
        data = data or b""
        try:
            if self.fd is None:
                self.fd = self._open()
            self.seq += 1
            os.write(self.fd, f"@ESL {self.seq} {method} {path} {len(data)}\n".encode() + data)
            end = time.monotonic() + self.timeout
            prefix = f"@ESL {self.seq} ".encode()
            body = b""
            while True:
                # A long reply (GET /api/tags) comes in pieces: every piece but the
                # last has "+" after its length, and log lines may come between them
                while True:
                    line = self._readline(end)
                    i = line.find(prefix)
                    if i >= 0:
                        break
                head = line[i + len(prefix):].split(b" ", 2)
                status, length = int(head[0]), head[1]
                more = length.endswith(b"+")
                length = int(length.rstrip(b"+"))
                # The body may contain newlines: take exactly length bytes
                payload = (head[2] if len(head) > 2 else b"")
                if len(payload) < length + 1:
                    payload += self._read(length + 1 - len(payload), end)
                body += payload[:length]
                if not more:
                    return status, body
        except (OSError, TimeoutError) as e:
            if self.fd is not None:  # the port is opened again next time (the AP may have restarted)
                try:
                    os.close(self.fd)
                except OSError:
                    pass
                self.fd = None
            raise ConnectionError(f"no answer on {self.port}: {e}") from None


# ---- bridge for web pages --------------------------------------------------------------

BRIDGE_ORIGINS = ("https://mathiasbk.github.io",)
LOCAL_ORIGIN = re.compile(r"^http://(localhost|127\.0\.0\.1|\[::1\])(:\d+)?$")


def origin_allowed(origin, extra=()):
    return origin in BRIDGE_ORIGINS or origin in extra or bool(LOCAL_ORIGIN.match(origin))


def make_bridge(cli, extra_origins=()):
    """An HTTP handler that passes /api/ requests on to cli (over WiFi or the USB port),
    so a web page (the API tab of the project page) can try the API. Only pages from
    BRIDGE_ORIGINS, localhost and extra_origins may use it."""
    from http.server import BaseHTTPRequestHandler
    lock = threading.Lock()

    class Bridge(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def _origin(self):
            """The Origin to allow, "" for no Origin (curl), None for a foreign page."""
            origin = self.headers.get("Origin")
            if origin is None:
                return ""
            return origin if origin_allowed(origin, extra_origins) else None

        def _reply(self, status, body=b"", origin="", extra=()):
            self.send_response(status)
            if origin:
                self.send_header("Access-Control-Allow-Origin", origin)
                self.send_header("Vary", "Origin")
            for k, val in extra:
                self.send_header(k, val)
            if body:
                self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_OPTIONS(self):
            origin = self._origin()
            if origin is None:
                return self._reply(403)
            self._reply(204, origin=origin, extra=(
                ("Access-Control-Allow-Methods", "GET, PUT, DELETE"),
                ("Access-Control-Allow-Headers", "Authorization, Content-Type"),
                ("Access-Control-Allow-Private-Network", "true"),
                ("Access-Control-Max-Age", "600")))

        def _forward(self):
            origin = self._origin()
            if origin is None:
                return self._reply(403, b'{"error":"this page may not use the bridge"}')
            if not self.path.startswith("/api/"):
                return self._reply(404, b'{"error":"not found"}', origin)
            n = int(self.headers.get("Content-Length") or 0)
            data = self.rfile.read(n) if n else None
            try:
                with lock:
                    status, payload = cli.send(self.command, self.path, data, self.headers.get("Authorization"))
            except ConnectionError as e:
                status, payload = 502, json.dumps({"error": str(e)}).encode()
            self._reply(status, payload, origin)

        do_GET = do_PUT = do_DELETE = do_POST = _forward

        def log_message(self, fmt, *args):
            print(f"{self.command} {self.path}: {args[1] if len(args) > 1 else ''}", file=sys.stderr)

    return Bridge


def serve(cli, port, extra_origins=()):
    from http.server import ThreadingHTTPServer
    httpd = ThreadingHTTPServer(("127.0.0.1", port), make_bridge(cli, extra_origins))
    print(f"bridge to {cli.url} on http://localhost:{port} (Ctrl+C stops it)", file=sys.stderr)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


def norm_mac(s):
    mac = s.replace(":", "").replace("-", "").upper()
    if not MAC_RE.match(mac):
        raise argparse.ArgumentTypeError(f"not a 16-digit hex MAC: {s}")
    return mac


def as_bool(s):
    return str(s).strip().lower() in ("1", "true", "yes", "ja", "x")


def parse_when(s):
    """--at / the at column: "2026-10-19 07:00" or "2026-10-19" (local time), ISO
    with an offset ("2026-10-19T07:00+02:00"), or Unix seconds. Returns Unix seconds."""
    s = str(s).strip()
    if s.isdigit():
        return int(s)
    try:
        d = datetime.datetime.fromisoformat(s)
    except ValueError:
        raise argparse.ArgumentTypeError(f"not a time: {s!r} (use e.g. 2026-10-19 07:00)")
    return int(d.timestamp())  # a time without offset is local time


def fmt_when(unix_s):
    return datetime.datetime.fromtimestamp(unix_s).strftime("%Y-%m-%d %H:%M")


def content(layout, fields, highlight=False, frame=False):
    c = {"layout": layout, "fields": list(fields)}
    if highlight:
        c["price_highlight"] = True
    if frame:
        c["frame"] = True
    return c


class CsvError(Exception):
    """A problem in a CSV file, with file and line."""


def read_text(path):
    """A CSV export as text: UTF-8 (with or without BOM), else Windows-1252, which
    is what Excel writes for "CSV" on a Norwegian Windows."""
    with open(path, "rb") as f:
        raw = f.read()
    try:
        return raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        return raw.decode("cp1252", "replace")


def read_table(path):
    """Header (lower case) and [(line, cells)] from a CSV file. The separator is
    the one of , ; or tab that the header line has most of (Excel uses ; where
    the decimal mark is a comma)."""
    text = read_text(path)
    first = text.split("\n", 1)[0]
    delim = max(",;\t", key=first.count)
    r = csv.reader(io.StringIO(text, newline=""), delimiter=delim)
    header = next(r, None)
    if not header:
        raise CsvError(f"{path}: empty file")
    header = [h.strip().lower() for h in header]
    rows = []
    for row in r:
        if any(c.strip() for c in row):
            rows.append((r.line_num, row))
    return header, rows


def read_bindings(path):
    """tags.csv for --tags: which tags show which item. {item: [mac, ...]}."""
    header, rows = read_table(path)
    if "mac" not in header or "item" not in header:
        raise CsvError(f"{path}: the header needs the columns mac and item")
    im, ii = header.index("mac"), header.index("item")
    by_item, seen = {}, {}
    for n, row in rows:
        cell = lambda i: row[i].strip() if i < len(row) else ""
        try:
            mac = norm_mac(cell(im))
        except argparse.ArgumentTypeError as e:
            raise CsvError(f"{path}:{n}: {e}")
        item = cell(ii)
        if not item:
            raise CsvError(f"{path}:{n}: no item for {mac}")
        if mac in seen:
            raise CsvError(f"{path}:{n}: {mac} is already bound to item {seen[mac]!r}")
        seen[mac] = item
        by_item.setdefault(item, []).append(mac)
    return by_item


def read_csv(path, layout=None, bindings=None, at=None):
    """Tag content from a CSV file: [(where, content with "mac")], where = "file:line".

    Columns are found by name: mac (or item, with bindings from --tags), layout
    (or the layout argument), highlight and frame are optional, and every other
    column is a field, in column order. An at column (or the at argument, Unix
    seconds) schedules the row. Problems are collected and raised together, so
    nothing is sent from a file with errors."""
    header, rows = read_table(path)
    key = "mac" if "mac" in header else "item" if "item" in header else None
    errors = []
    if key is None:
        raise CsvError(f"{path}: the header needs a mac column, or an item column together with --tags")
    if key == "item" and bindings is None:
        raise CsvError(f"{path}: an item column needs --tags with the file that binds tags to items")
    if "layout" not in header and not layout:
        raise CsvError(f"{path}: no layout column: add one, or give --layout")
    col = {name: header.index(name) for name in ("layout", "highlight", "frame", "at") if name in header}
    col[key] = header.index(key)
    fields = [i for i, h in enumerate(header) if i not in col.values()]

    out, where_mac, items = [], {}, set()
    for n, row in rows:
        where = f"{path}:{n}"
        cell = lambda name: row[col[name]].strip() if name in col and col[name] < len(row) else ""
        lay = cell("layout") or layout
        if not lay:
            errors.append(f"{where}: no layout")
            continue
        c = content(lay, [row[i] if i < len(row) else "" for i in fields],
                    as_bool(cell("highlight")), as_bool(cell("frame")))
        while c["fields"] and not c["fields"][-1]:
            c["fields"].pop()
        when = at
        if cell("at"):
            try:
                when = parse_when(cell("at"))
            except argparse.ArgumentTypeError as e:
                errors.append(f"{where}: {e}")
                continue
        if when:
            c["at"] = when
        if key == "mac":
            try:
                macs = [norm_mac(cell("mac"))]
            except argparse.ArgumentTypeError as e:
                errors.append(f"{where}: {e}")
                continue
        else:
            item = cell("item")
            if (item, when) in items:
                errors.append(f"{where}: item {item!r} is in the file twice" + (" for the same time" if when else ""))
                continue
            items.add((item, when))
            macs = bindings.get(item, [])
        for mac in macs:
            if (mac, when) in where_mac:
                errors.append(f"{where}: {mac} already has content from line "
                              f"{where_mac[mac, when].rsplit(':', 1)[1]}" + (" for the same time" if when else ""))
                continue
            where_mac[mac, when] = where
            out.append((where, dict(c, mac=mac)))
    if errors:
        raise CsvError("\n".join(errors))
    unused = []
    if key == "item":
        unused = sorted(set(bindings) - {i for i, _ in items})
    return out, unused


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
          f"tags {len(st.get('tags', []))}/{ap.get('max_tags')}"
          + (f"  reset {ap['reset']}" if ap.get("reset") else "")
          + (f"  scheduled {ap['scheduled']}" if ap.get("scheduled") else ""))
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
              f"{(str(batt) + ' mV') if batt else '-':>6}  "
              f"{t.get('rssi') if t.get('rssi') is not None else '-':>4}  {state}")


def print_health(h):
    """One line per tag with a problem, and the counts. Returns 0 when everything is fine."""
    counts = ", ".join(f"{k} {h.get(k)}" for k in ("silent", "unseen", "draw_failed", "low_battery",
                                                   "layout_failed", "content_error") if h.get(k))
    print(f"{'ok' if h.get('ok') else 'PROBLEMS'}: {h.get('with_problems')} of {h.get('tags')} tags"
          + (f" ({counts})" if counts else ""))
    for t in h.get("problems", []):
        print(f"  {t.get('mac')}  {', '.join(t.get('problems', [])):28}  seen {fmt_ago(t.get('seen_s_ago')):>7}"
              + (f"  {t['battery_mv']} mV" if t.get("battery_mv") else ""))
    return 0 if h.get("ok") else 1


def print_config(c):
    print(f"poll_s {c.get('poll_s')} (menuconfig {c.get('builtin_poll_s')}), webhook {c.get('webhook') or 'off'}")


def print_ota(o):
    other = f"{o.get('other_slot')} holds {o.get('other_version') or 'nothing'}" if o.get("other_slot") else "no second slot"
    print(f"running {o.get('version')} from {o.get('slot')}, slot size {o.get('slot_size')} bytes, {other}"
          + (", upload in progress" if o.get("uploading") else ""))


def print_tag_firmware(f):
    for i in f.get("images", []):
        if not i.get("version"):
            print(f"{i['model']:>5}: no firmware")
            continue
        t = i.get("tags", {})
        print(f"{i['model']:>5}: {i['version']} ({i['size']} bytes, CRC {i['crc']}), tags: {t.get('updated', 0)} updated, "
              f"{t.get('pending', 0)} pending, {t.get('failed', 0)} failed, {t.get('no_ota', 0)} without OTA")


def tag_firmware_header(data):
    """(model name, "x.y.z") from a tag firmware file, or None if it is not one."""
    if len(data) < 16 or data[:4] != TAGFW_MAGIC:
        return None
    model = {1: "2.9", 2: "1.54"}.get(data[5], f"model {data[5]}")
    v = int.from_bytes(data[6:8], "little")
    return model, f"{v // 10000}.{v // 100 % 100}.{v % 100}"


def wait_back(cli, slot, version, timeout_s=120, poll_s=2, sleep=time.sleep):
    """After an OTA: asks GET /api/ota until the AP answers again, and says
    whether it runs the new image or fell back to the old slot. Returns 0 when
    the new one runs, 1 otherwise."""
    end = time.monotonic() + timeout_s
    while True:
        try:
            o = cli.ota_info()
        except ConnectionError:
            o = None
        if o is not None:
            if o.get("slot") == slot and o.get("version") == version:
                print(f"the AP is back: version {version} from {slot}")
                return 0
            print(f"the AP is back on {o.get('slot')} with version {o.get('version')}: "
                  f"the new image did not confirm itself, so the bootloader fell back to the old one")
            return 1
        if time.monotonic() >= end:
            print(f"no answer from the AP after {timeout_s} s; check its log on the USB port", file=sys.stderr)
            return 1
        sleep(poll_s)


def print_wifi(w):
    line = f"WiFi {w.get('mode')}: "
    if w.get("connected"):
        line += f"connected to {w.get('ssid')!r}, IP {w.get('ip')}, {w.get('rssi')} dBm"
    elif w.get("ssid"):
        line += f"trying to join {w.get('ssid')!r}"
    else:
        line += "no network saved"
    if w.get("setup_ssid"):
        line += f"; setup network {w['setup_ssid']!r} open (http://192.168.4.1/setup)"
    print(line)
    for n in w.get("networks", []):
        print(f"  {n.get('rssi'):>4} dBm  {n.get('ssid')}")


def ensure_time(cli):
    """Before scheduling: the AP must know the time. Sets it from this computer if
    it does not (an AP without WiFi has no NTP); warns if the clocks differ."""
    t = cli.time()
    now = int(time.time())
    if t.get("time") is None:
        cli.set_time(now)
        print("note: the AP did not know the time; set it from this computer")
    elif abs(t["time"] - now) > 120:
        print(f"warning: the AP's clock ({t.get('source')}) is {t['time'] - now:+d} s off this computer's; "
              "scheduled content follows the AP's clock", file=sys.stderr)


def print_result(r):
    if r.get("scheduled"):
        print(f"{r['mac']}: scheduled for {fmt_when(r['at'])}")
    else:
        print(f"{r['mac']}: content_id {r['content_id']}, "
              f"{'changed' if r.get('changed') else 'unchanged'}, {'pending' if r.get('pending') else 'shown'}")


def print_schedule(sc):
    rows = sorted(sc.get("scheduled", []), key=lambda e: (e["at"], e["mac"]))
    for e in rows:
        print(f"{fmt_when(e['at'])}  {e['mac']}  {e.get('layout', ''):10}  {' | '.join(e.get('fields', []))}")
    print(f"{len(rows)} of {sc.get('max_scheduled')} scheduled"
          + ("" if sc.get("time") else "; the AP does not know the time, so nothing is applied"))


# ---- long-term test: log and report -------------------------------------------------------

LOG_COLUMNS = ["time", "ap_uptime_s", "ap_reset", "mac", "model", "seen_s_ago", "battery_mv", "rssi",
               "content_id", "shown_content_id", "pending", "draw_failed", "problems"]


def log_round(cli, writer, now=None):
    """One round of the long-term test: a row per tag from GET /api/tags, with the
    problems GET /api/health lists for it. When the AP does not answer, one row
    with an empty mac is written, so the outage is in the log too. Returns a one-line
    summary."""
    now = now or time.time()
    stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(now))
    try:
        st = cli.tags()
        problems = {t["mac"]: t.get("problems", []) for t in cli.health().get("problems", [])}
    except (ApiError, ConnectionError) as e:
        writer.writerow([stamp, "", "no answer", "", "", "", "", "", "", "", "", "", str(e)[:80]])
        return f"{stamp}  AP did not answer: {e}"
    ap = st.get("ap", {})
    tags = st.get("tags", [])
    for t in tags:
        writer.writerow([stamp, ap.get("uptime_s", ""), ap.get("reset", ""), t["mac"], t.get("model") or "",
                         t.get("seen_s_ago", ""), t.get("battery_mv", ""), t.get("rssi", ""),
                         t.get("content_id", ""), t.get("shown_content_id", ""),
                         int(bool(t.get("pending"))), int(bool(t.get("draw_failed"))),
                         "|".join(problems.get(t["mac"], []))])
    if not tags:
        writer.writerow([stamp, ap.get("uptime_s", ""), ap.get("reset", ""), "", "", "", "", "", "", "", "", "", ""])
    with_problems = sum(1 for t in tags if problems.get(t["mac"]))
    return (f"{stamp}  {len(tags)} tags, {with_problems} with problems, AP up {fmt_ago(ap.get('uptime_s'))}"
            + (f", reset {ap['reset']}" if ap.get("reset") else ""))


def log_loop(cli, path, every_s, rounds=None, sleep=time.sleep):
    """Appends a round to path every every_s seconds until Ctrl-C (or rounds rounds)."""
    new = not os.path.exists(path) or os.path.getsize(path) == 0
    with open(path, "a", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        if new:
            w.writerow(LOG_COLUMNS)
        n = 0
        try:
            while rounds is None or n < rounds:
                started = time.monotonic()
                print(log_round(cli, w), flush=True)
                f.flush()
                n += 1
                if rounds is not None and n >= rounds:
                    break
                sleep(max(0.0, every_s - (time.monotonic() - started)))
        except KeyboardInterrupt:
            print(f"stopped after {n} rounds; report with: esl_client.py report {path}")
    return 0


def read_log(path):
    with open(path, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        if "mac" not in r:
            raise CsvError(f"{path}: not a log from esl_client.py log (no mac column)")
    return rows


def report(rows):
    """Per tag: samples, the longest time without a poll, lowest battery, rounds with
    problems and with a failed drawing; and for the AP: rounds without an answer and
    restarts (uptime went down). Returns (lines, ok)."""
    def num(v):
        try:
            return int(v)
        except (TypeError, ValueError):
            return None

    outages = [r for r in rows if r["mac"] == "" and r["ap_reset"] == "no answer"]
    answered = [r for r in rows if r["ap_reset"] != "no answer"]
    rounds = sorted({r["time"] for r in answered})
    restarts = []
    last_up = None
    for stamp in rounds:
        up = next((num(r["ap_uptime_s"]) for r in answered if r["time"] == stamp), None)
        if up is not None and last_up is not None and up < last_up:
            restarts.append(stamp)
        last_up = up if up is not None else last_up

    per = {}
    for r in answered:
        if not r["mac"]:
            continue
        t = per.setdefault(r["mac"], {"model": r["model"], "n": 0, "gap": 0, "batt": None, "problems": 0,
                                       "draw_failed": 0, "last_problems": ""})
        t["n"] += 1
        gap = num(r["seen_s_ago"])
        if gap is not None and gap > t["gap"]:
            t["gap"] = gap
        b = num(r["battery_mv"])
        if b and (t["batt"] is None or b < t["batt"]):
            t["batt"] = b
        if r["problems"]:
            t["problems"] += 1
            t["last_problems"] = r["problems"]
        if r["draw_failed"] == "1":
            t["draw_failed"] += 1

    span = f"{rows[0]['time']} to {rows[-1]['time']}" if rows else "empty log"
    lines = [f"{len(rounds)} rounds, {span}; AP did not answer {len(outages)} times, restarted {len(restarts)} times"
             + (f" ({', '.join(restarts)})" if restarts else "")]
    lines.append(f"{'MAC':16}  {'model':5}  {'rounds':>6}  {'max gap':>8}  {'min batt':>8}  {'problems':>8}  {'draw fail':>9}  last problem")
    ok = not outages and not restarts
    for mac, t in sorted(per.items()):
        lines.append(f"{mac:16}  {t['model'] or '-':5}  {t['n']:>6}  {fmt_ago(t['gap']):>8}  "
                     f"{(str(t['batt']) + ' mV') if t['batt'] else '-':>8}  {t['problems']:>8}  {t['draw_failed']:>9}  "
                     f"{t['last_problems']}")
        if t["problems"] or t["draw_failed"]:
            ok = False
    return lines, ok


def set_one(cli, args, c):
    if args.at:
        ensure_time(cli)
        c["at"] = args.at
    r = cli.set_tag(args.mac, c)
    print_result(r)
    if args.wait and r.get("pending"):
        print("waiting for the tag to show it ...", flush=True)
        ok = cli.wait_shown(args.mac, r["content_id"], timeout_s=args.wait_s)
        print("shown" if ok else f"not shown after {args.wait_s} s")
        return 0 if ok else 1
    return 0


def sync(cli, args):
    try:
        bindings = read_bindings(args.tags) if args.tags else None
        rows, unused = read_csv(args.csv, args.layout, bindings, args.at)
    except (CsvError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    for item in unused:
        print(f"note: item {item!r} in {args.tags} is not in {args.csv}; its tags are left as they are")
    if args.dry_run:
        for where, c in rows:
            when = f"  from {fmt_when(c['at'])}" if c.get("at") else ""
            print(f"{c['mac']}  {c['layout']:10}  {' | '.join(c['fields'])}{when}  ({where})")
        print(f"{len(rows)} tags would be sent")
        return 0
    if any(c.get("at") for _, c in rows):
        ensure_time(cli)
    res = []
    for i in range(0, len(rows), BATCH_MAX):
        chunk = rows[i:i + BATCH_MAX]
        try:
            res.extend(cli.set_many([c for _, c in chunk]))
        except ApiError as e:
            msg = e.body
            try:
                err = json.loads(e.body)
                msg = err.get("error", msg)
                if isinstance(err.get("index"), int) and 0 <= err["index"] < len(chunk):
                    msg = f"{chunk[err['index']][0]}: {msg}"
            except ValueError:
                pass
            print(f"error: HTTP {e.status}: {msg}", file=sys.stderr)
            if res:
                print(f"the {len(res)} tags before this group of {len(chunk)} were sent; "
                      "fix the line and run sync again (unchanged tags cost nothing)", file=sys.stderr)
            return 2
    changed = sum(1 for r in res if r.get("changed"))
    scheduled = sum(1 for r in res if r.get("scheduled"))
    print(f"{len(res)} tags sent, {changed} changed, {sum(1 for r in res if r.get('pending'))} pending"
          + (f", {scheduled} scheduled" if scheduled else ""))
    return 0


# ---- backup and restore ----------------------------------------------------------------------

BACKUP_FORMAT = "openesl-backup"
MODELS = ("2.9", "1.54")
CONTENT_KEYS = ("layout", "fields", "price_highlight", "frame")


def backup(cli, now=None):
    """Everything the AP keeps in NVS that the API can read back: the default poll
    interval and the webhook, the layout files, each tag's content and settings, and
    the scheduled content. Not the WiFi (its password cannot be read) or the token."""
    st = cli.tags()
    tags = []
    for t in st.get("tags", []):
        cfg = {k: v for k, v in (t.get("config") or {}).items() if v is not None}
        row = {"mac": t["mac"]}
        if t.get("content_id", 0):
            d = cli.tag(t["mac"])
            row.update({k: d[k] for k in CONTENT_KEYS if d.get(k) is not None})
        if cfg:
            row["config"] = cfg
        if len(row) > 1:  # a tag that has only polled is added back when it polls again
            tags.append(row)
    cfg = cli.config()
    layouts = {m: f for m in MODELS if (f := cli.layout(m)) is not None}
    try:
        sched = [{k: v for k, v in e.items() if v is not None} for e in cli.schedule().get("scheduled", [])]
    except ApiError as e:
        if e.status != 404:  # an AP built without scheduling
            raise
        sched = []
    sched.sort(key=lambda e: (e["at"], e["mac"]))
    return {"format": BACKUP_FORMAT, "version": 1,
            "taken": datetime.datetime.fromtimestamp(now or time.time(), datetime.timezone.utc)
            .isoformat(timespec="seconds"),
            "ap": {"fw": st.get("ap", {}).get("fw"), "channel": st.get("ap", {}).get("channel")},
            "config": {"poll_s": cfg.get("poll_s", 0), "webhook": cfg.get("webhook")},
            "layouts": layouts, "tags": tags, "scheduled": sched}


class BackupError(Exception):
    pass


def read_backup(path):
    try:
        with open(path, "rb") as f:
            b = json.load(f)
    except ValueError as e:
        raise BackupError(f"{path}: not JSON ({e})")
    if not isinstance(b, dict) or b.get("format") != BACKUP_FORMAT or b.get("version") != 1:
        raise BackupError(f"{path}: not a backup from \"esl-client backup\"")
    for key, kind in (("config", dict), ("layouts", dict), ("tags", list), ("scheduled", list)):
        if not isinstance(b.get(key), kind):
            raise BackupError(f"{path}: \"{key}\" is missing or not a {kind.__name__}")
    for i, t in enumerate(b["tags"] + b["scheduled"]):
        if not isinstance(t, dict) or not MAC_RE.match(str(t.get("mac", ""))):
            raise BackupError(f"{path}: entry {i} has no valid mac")
        if "layout" in t and not isinstance(t.get("fields"), list):
            raise BackupError(f"{path}: {t['mac']} has a layout but no fields")
    return b


def restore(cli, b, dry_run=False, now=None):
    """Puts a backup onto an AP, in the order the AP needs: config, layout files, tag
    settings, content, then the scheduled content (entries whose time has passed are
    skipped, since the AP would set them now). Tags the backup does not have are left
    as they are. Returns (lines, ok)."""
    now = int(now or time.time())
    lines = []
    ok = True
    cfg = b["config"]
    want = {"poll_s": cfg.get("poll_s", 0)}
    if cfg.get("webhook"):
        want["webhook"] = cfg["webhook"]
    layouts = {m: f for m, f in b["layouts"].items() if m in MODELS}
    settings = [(t["mac"], t["config"]) for t in b["tags"] if t.get("config")]
    content = [{"mac": t["mac"], **{k: t[k] for k in CONTENT_KEYS if k in t}} for t in b["tags"] if "layout" in t]
    due = [e for e in b["scheduled"] if e["at"] > now]
    passed = len(b["scheduled"]) - len(due)
    lines.append(f"config: poll_s {want['poll_s']}, webhook {want.get('webhook') or 'off'}")
    lines.append(f"layout files: {', '.join(layouts) or 'none'}")
    lines.append(f"tags: {len(content)} with content, {len(settings)} with their own settings")
    lines.append(f"scheduled: {len(due)}" + (f" ({passed} skipped, their time has passed)" if passed else ""))
    if dry_run:
        lines.append("nothing sent (dry run)")
        return lines, True

    try:
        cli.set_config(**want)
    except ApiError as e:
        if "webhook" not in want or e.status != 422:
            raise
        lines.append(f"warning: the webhook was not restored ({e.body.strip()}); this AP has no webhook")
        cli.set_config(poll_s=want["poll_s"])
        ok = False
    for m, f in layouts.items():
        cli.put_layout(m, json.dumps(f).encode("utf-8"))
    for mac, c in settings:
        cli.set_tag_config(mac, c)
    sent = 0
    for i in range(0, len(content), BATCH_MAX):
        chunk = content[i:i + BATCH_MAX]
        try:
            sent += len(cli.set_many(chunk))
        except ApiError as e:
            msg = e.body
            try:
                err = json.loads(e.body)
                msg = err.get("error", msg)
                if isinstance(err.get("index"), int) and 0 <= err["index"] < len(chunk):
                    msg = f"{chunk[err['index']]['mac']}: {msg}"
            except ValueError:
                pass
            lines.append(f"error: HTTP {e.status}: {msg}; {sent} tags were restored before it")
            return lines, False
    if due:
        rows = [{"mac": e["mac"], "at": e["at"], **{k: e[k] for k in CONTENT_KEYS if k in e}} for e in due]
        try:
            ensure_time(cli)
            cli.set_many(rows)
        except ApiError as e:
            if e.status != 404:
                raise
            lines.append(f"warning: the {len(due)} scheduled entries were not restored; this AP has no scheduling")
            due = []
            ok = False
    lines.append(f"restored: {len(layouts)} layout files, {len(settings)} tag settings, {sent} tags, {len(due)} scheduled")
    return lines, ok


def main(argv=None):
    p = argparse.ArgumentParser(description="OpenESL AP example client")
    p.add_argument("--url", default=os.environ.get("OPENESL_URL", "http://openesl.local"))
    p.add_argument("--token", default=os.environ.get("OPENESL_TOKEN", ""))
    p.add_argument("--serial", metavar="PORT", default=os.environ.get("OPENESL_SERIAL", ""),
                   help="talk to the AP over its USB port, e.g. /dev/ttyACM0")
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
        s.add_argument("--at", type=parse_when, metavar="WHEN",
                       help='schedule it: "2026-10-19 07:00" (local time), ISO with offset, or Unix seconds')
        s.add_argument("--wait", action="store_true", help="wait until the tag shows it")
        s.add_argument("--wait-s", type=int, default=120)

    s = sub.add_parser("sync", help="full send from CSV (chunks of 50)")
    s.add_argument("csv")
    s.add_argument("--tags", metavar="CSV", help="mac,item file: the CSV has an item column instead of mac")
    s.add_argument("--layout", help="layout for rows without one (or when there is no layout column)")
    s.add_argument("--dry-run", action="store_true", help="check the files and show what would be sent")
    s.add_argument("--at", type=parse_when, metavar="WHEN",
                   help="schedule every row without its own at column, e.g. \"2026-10-19 07:00\"")
    sub.add_parser("schedule", help="list scheduled content")
    s = sub.add_parser("schedule-clear", help="remove scheduled content, for one tag or all")
    s.add_argument("mac", type=norm_mac, nargs="?")
    s = sub.add_parser("time", help="show the AP's clock, or set it from this computer")
    s.add_argument("--set", action="store_true")
    s = sub.add_parser("delete", help="remove a tag from the AP")
    s.add_argument("mac", type=norm_mac)
    sub.add_parser("layouts", help="list layout files per model")
    s = sub.add_parser("layout-put", help="upload a layout file for a model")
    s.add_argument("model", choices=["2.9", "1.54"])
    s.add_argument("file")
    s = sub.add_parser("serve", help="bridge on localhost so the API tab of the web page can try the API")
    s.add_argument("--port", type=int, default=8080)
    s.add_argument("--allow-origin", action="append", default=[], metavar="ORIGIN",
                   help="another page that may use the bridge, e.g. http://192.168.1.10:8000")

    s = sub.add_parser("health", help="tags that need attention (exit code 1 when there are any)")
    s.add_argument("--silent-s", type=int, help="seconds without a poll before a tag is silent (default 3 x its interval)")
    s.add_argument("--battery-mv", type=int, help="battery below this is low (default 2500)")
    s = sub.add_parser("config", help="show or set the AP's default poll interval and webhook")
    s.add_argument("--poll-s", help="default interval in s, 0 = tags keep their own, \"null\" = menuconfig value")
    s.add_argument("--webhook", help="URL the AP posts the health report to when it changes; \"\" turns it off")

    s = sub.add_parser("ota", help="send new AP firmware (the AP restarts into it), or show the app slots")
    s.add_argument("image", nargs="?", help="build/esl_ap.bin, or openesl-ap-*-ota.bin from a release")
    s.add_argument("--wait", action="store_true", help="wait until the AP answers again and check that the new image runs")
    s.add_argument("--wait-s", type=int, default=120)

    s = sub.add_parser("tag-ota", help="send new tag firmware to the AP, which updates the tags over the radio; "
                                       "alone, show what the AP has and how far the tags are")
    s.add_argument("file", nargs="?", help="openesl-tag-<model>-<version>-ota.bin from a release, or tag/build/fw/openesl-ota.bin")
    s.add_argument("--delete", metavar="MODEL", help="stop updating tags of this model (2.9 or 1.54)")

    s = sub.add_parser("log", help="long-term test: append a row per tag to a CSV file every --every seconds, until Ctrl-C")
    s.add_argument("file", help="the CSV log (created with a header if new, otherwise appended to)")
    s.add_argument("--every", type=float, default=60, metavar="S", help="seconds between rounds (default 60)")
    s.add_argument("--rounds", type=int, help="stop after this many rounds (default: run until Ctrl-C)")
    s = sub.add_parser("report", help="summarise a log from \"log\": gaps, battery, problems, AP restarts (exit 1 on any)")
    s.add_argument("file")

    s = sub.add_parser("backup", help="save the AP's tags, settings, layout files and schedule to a JSON file")
    s.add_argument("file", help="where to write it (- for stdout)")
    s = sub.add_parser("restore", help="put a backup from \"backup\" onto an AP (a new one, or after erase-flash)")
    s.add_argument("file")
    s.add_argument("--dry-run", action="store_true", help="show what would be restored")

    s = sub.add_parser("wifi", help="show the AP's WiFi, or set the network it joins")
    s.add_argument("ssid", nargs="?", help="network to join; \"\" forgets the saved one")
    s.add_argument("--password", default="", help="empty for an open network")

    args = p.parse_args(argv)
    if args.cmd == "report":
        try:
            lines, ok = report(read_log(args.file))
        except (OSError, CsvError) as e:
            print(f"error: {e}", file=sys.stderr)
            return 2
        print("\n".join(lines))
        return 0 if ok else 1
    if args.serial:
        cli = SerialClient(args.serial)
    elif not args.token and args.cmd != "serve":
        p.error("no token: use --token or OPENESL_TOKEN")
    else:
        cli = Client(args.url, args.token)

    if args.cmd == "serve":
        return serve(cli, args.port, tuple(args.allow_origin))
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
            return sync(cli, args)
        elif args.cmd == "schedule":
            print_schedule(cli.schedule())
        elif args.cmd == "schedule-clear":
            try:
                n = cli.clear_schedule(args.mac)["deleted"]
            except ApiError as e:
                if e.status != 404:
                    raise
                n = 0
            print(f"{n} scheduled entries removed")
        elif args.cmd == "time":
            t = cli.set_time(time.time()) if args.set else cli.time()
            if t.get("time") is None:
                print("the AP does not know the time (no NTP yet); time --set sets it from this computer")
            else:
                print(f"{fmt_when(t['time'])} ({t['source']}), {t['time'] - int(time.time()):+d} s from this computer")
        elif args.cmd == "delete":
            cli.delete_tag(args.mac)
            print(f"{args.mac} deleted")
        elif args.cmd == "health":
            return print_health(cli.health(args.silent_s, args.battery_mv))
        elif args.cmd == "config":
            kw = {}
            if args.poll_s is not None:
                kw["poll_s"] = None if args.poll_s == "null" else int(args.poll_s)
            if args.webhook is not None:
                kw["webhook"] = args.webhook or None
            print_config(cli.set_config(**kw) if kw else cli.config())
        elif args.cmd == "log":
            return log_loop(cli, args.file, args.every, args.rounds)
        elif args.cmd == "ota":
            if args.image is None:
                print_ota(cli.ota_info())
            else:
                with open(args.image, "rb") as f:
                    image = f.read()
                if image[:1] != b"\xe9":
                    print(f"error: {args.image} is not an ESP32 app image (use esl_ap.bin, not the merged flash image)",
                          file=sys.stderr)
                    return 2
                print(f"sending {len(image)} bytes ...", flush=True)
                r = cli.ota(image, lambda sent, total: print(f"  {sent * 100 // total} %", end="\r", flush=True))
                print(f"written to {r.get('slot')}: version {r.get('version')}, the AP is restarting")
                if args.wait:
                    print("waiting for the AP to come back ...", flush=True)
                    sleep_s = 3  # the restart itself takes a few seconds
                    time.sleep(sleep_s)
                    return wait_back(cli, r.get("slot"), r.get("version"), args.wait_s)
        elif args.cmd == "tag-ota":
            if args.delete:
                cli.delete_tag_firmware(args.delete)
                print(f"no more firmware updates for {args.delete} tags")
            elif args.file is None:
                print_tag_firmware(cli.tag_firmware())
            else:
                with open(args.file, "rb") as f:
                    data = f.read()
                hdr = tag_firmware_header(data)
                if not hdr:
                    print(f"error: {args.file} is not a tag firmware file (use openesl-ota.bin or openesl-tag-<model>-<version>-ota.bin, "
                          f"not openesl.bin, which is for the flasher)", file=sys.stderr)
                    return 2
                print(f"sending {hdr[0]} firmware {hdr[1]} ({len(data)} bytes) ...", flush=True)
                r = cli.put_tag_firmware(data, lambda sent, total: print(f"  {sent * 100 // total} %", end="\r",
                                                                        flush=True))
                print(f"the AP offers {r.get('model')} firmware {r.get('version')} to the tags; "
                      f"each takes it at its next poll (\"tag-ota\" shows how far they are)")
        elif args.cmd == "backup":
            b = backup(cli)
            text = json.dumps(b, indent=1, ensure_ascii=False)
            if args.file == "-":
                print(text)
            else:
                with open(args.file, "w", encoding="utf-8") as f:
                    f.write(text + "\n")
                print(f"{args.file}: {len(b['tags'])} tags, {len(b['layouts'])} layout files, "
                      f"{len(b['scheduled'])} scheduled")
        elif args.cmd == "restore":
            try:
                b = read_backup(args.file)
            except (OSError, BackupError) as e:
                print(f"error: {e}", file=sys.stderr)
                return 1
            lines, ok = restore(cli, b, args.dry_run)
            print("\n".join(lines))
            return 0 if ok else 1
        elif args.cmd == "wifi":
            if args.ssid is None:
                print_wifi(cli.wifi())
            else:
                print_wifi(cli.set_wifi(args.ssid, args.password))
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
