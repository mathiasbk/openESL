"""Tests for esl_client.py against a small fake AP (python3 -m unittest)."""

import base64
import datetime
import csv
import glob
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
    tag_cfg = {}
    layouts = {}
    ota = {}
    ota_rollback = False
    tagfw = {}
    ap_fw = "0.1.0"
    images = {}
    fonts = None   # name -> font file; None: an AP without font files (404)
    other_aps = []

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

    def _tagfw_model(self):
        """GET /api/tag-firmware/1.54 for the image uploaded to the fake (the only model it takes)."""
        if "to" not in FakeAp.tagfw:
            return None
        img = {"model": "1.54", "version": "0.3.0", "size": 53248, "crc": "1234ABCD", "to": FakeAp.tagfw["to"],
               "tags": FakeAp.tagfw.get("tags", {"updated": 0, "pending": 1, "failed": 0, "no_ota": 0,
                                                 "not_chosen": 3})}
        if FakeAp.tagfw.get("old_ap"):  # an AP from before "to" (0.4.4) has no "to" field
            del img["to"]
        return img

    def do_DELETE(self):
        if not self._auth():
            return
        self.calls.append((self.path, None))
        if self.path == "/api/tag-firmware/1.54" and "to" in FakeAp.tagfw:
            del FakeAp.tagfw["to"]
            return self._send(200, {"model": "1.54", "deleted": True})
        self._send(404, {"error": "no tag firmware for this model"})

    def do_POST(self):
        if not self._auth():
            return
        if self.path.startswith("/api/tag-firmware/"):
            self.calls.append((self.path, None))
            if FakeAp.tagfw.get("old_ap"):
                return self._send(405, {"error": "method not allowed"})
            to = self.path.split("?to=")[1]
            if self._tagfw_model() is None or not self.path.startswith("/api/tag-firmware/1.54?"):
                return self._send(404, {"error": "no tag firmware for this model"})
            if to == "all" or FakeAp.tagfw["to"] == "all":
                FakeAp.tagfw["to"] = "all"
            elif to not in FakeAp.tagfw["to"]:
                FakeAp.tagfw["to"].append(to)
            return self._send(200, self._tagfw_model())
        if self.path == "/api/ota":
            n = int(self.headers.get("Content-Length", 0))
            image = self.rfile.read(n)
            self.calls.append((self.path, self.headers.get("Content-Type"), image))
            if image[:1] != b"\xe9":
                return self._send(422, {"error": "the image is not valid for this chip"})
            if b"esl_radio" in image:  # passed on to the radio coprocessor
                FakeAp.ota["radio"] = dict(FakeAp.ota.get("radio") or {}, version="0.2.0")
                return self._send(200, {"received": n, "total": n, "radio": True, "version": "0.2.0", "restarting": False})
            if not FakeAp.ota_rollback:  # "restarted" into the new slot at once
                FakeAp.ota.update({"slot": "ota_1", "version": "0.2.0", "other_slot": "ota_0", "other_version": "0.1.0"})
            self._send(200, {"received": n, "total": n, "slot": "ota_1", "version": "0.2.0", "restarting": True})
        else:
            self._send(404, {"error": "not found"})

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
                          "model": "2.9", "seen_s_ago": 3, "battery_mv": 2950, "rssi": -60,
                          "config": self._cfg(mac), "layout": c.get("layout"), "fields": c.get("fields"),
                          "price_highlight": bool(c.get("price_highlight")), "frame": bool(c.get("frame"))}
        return {"mac": mac, "content_id": cid, "changed": changed, "pending": False}

    @staticmethod
    def _cfg(mac):
        c = FakeAp.tag_cfg.get(mac, {})
        return {k: c.get(k) for k in ("poll_s", "max_poll_s", "lost_ap_s", "long_poll_s", "long_after_min")}

    def do_GET(self):
        if not self._auth():
            return
        if self.path == "/api/layouts":
            self._send(200, {"max_size": 8192, "max_binary_size": 2048,
                             "layouts": [{"model": m, "version": 1, "size": 1, "binary_size": 1,
                                          "layouts": [x["name"] for x in f["layouts"]]}
                                         for m, f in FakeAp.layouts.items()]})
        elif self.path == "/api/fonts" and FakeAp.fonts is not None:
            self._send(200, {"max_fonts": 8, "max_bytes": 32768, "bytes": 0, "builtin": [],
                             "fonts": [{"name": n, "slot": i, "used_by": []} for i, n in enumerate(FakeAp.fonts)]})
        elif self.path.startswith("/api/fonts/") and FakeAp.fonts is not None:
            f = FakeAp.fonts.get(self.path.rsplit("/", 1)[1])
            self._send(200 if f else 404, f or {"error": "no font file"})
        elif self.path.startswith("/api/layouts/"):
            f = FakeAp.layouts.get(self.path.rsplit("/", 1)[1])
            self._send(200 if f else 404, f or {"error": "no layout file"})
        elif self.path.endswith("/config") and self.path.startswith("/api/tags/"):
            mac = self.path.split("/")[3]
            self._send(200, {"mac": mac, "poll_s": None, "config": self._cfg(mac), "config_pending": False})
        elif self.path == "/api/wifi":
            self._send(200, self.wifi)
        elif self.path == "/api/time":
            self._send(200, {"time": FakeAp.time, "source": "api" if FakeAp.time else None})
        elif self.path == "/api/schedule":
            self._send(200, {"time": FakeAp.time, "max_scheduled": 256, "scheduled": FakeAp.sched})
        elif self.path == "/api/config":
            self._send(200, self.config)
        elif self.path == "/api/ota":
            self._send(200, FakeAp.ota)
        elif self.path.startswith("/api/tag-firmware/"):
            img = self._tagfw_model() if self.path == "/api/tag-firmware/1.54" else None
            self._send(200 if img else 404, img or {"error": "no tag firmware for this model"})
        elif self.path == "/api/tag-firmware":
            self._send(200, {"max_size": 61456, "part_max": 24576, "images": FakeAp.tagfw.get("images", [
                {"model": "2.9", "version": None},
                self._tagfw_model() or
                {"model": "1.54", "version": "0.3.0", "size": 53248, "crc": "1234ABCD", "to": "all",
                 "tags": {"updated": 2, "pending": 1, "failed": 0, "no_ota": 1}}])})
        elif self.path.startswith("/api/health"):
            self.calls.append((self.path, None))
            problems = [{"mac": m, "problems": ["low_battery"], "seen_s_ago": t["seen_s_ago"],
                         "battery_mv": t["battery_mv"]} for m, t in self.tags.items() if t["battery_mv"] < 2500]
            self._send(200, {"problems": problems, "ok": not problems, "tags": len(self.tags),
                             "with_problems": len(problems), "silent": 0, "unseen": 0, "draw_failed": 0,
                             "low_battery": len(problems), "content_error": 0, "other_aps": FakeAp.other_aps})
        elif self.path == "/api/tags":
            self._send(200, {"ap": {"fw": FakeAp.ap_fw, "uptime_s": 10, "channel": 25, "max_tags": 64},
                             "tags": list(self.tags.values())})
        elif self.path.startswith("/api/tags/") and self.path.endswith("/image"):
            mac = self.path.split("/")[3]
            if mac not in FakeAp.images:
                return self._send(404, {"error": "the tag has no content"})
            self._send(200, {"mac": mac, "model": "2.9", "width": 296, "height": 128, "red": False,
                             "content_id": self.tags[mac]["content_id"], "image_id": 7,
                             "data": base64.b64encode(FakeAp.images[mac]).decode()})
        elif self.path.startswith("/api/tags/"):
            t = self.tags.get(self.path.rsplit("/", 1)[1])
            self._send(200 if t else 404, t or {"error": "not found"})
        else:
            self._send(404, {})

    def do_PUT(self):
        if not self._auth():
            return
        if self.path.startswith("/api/tag-firmware?"):
            n = int(self.headers.get("Content-Length", 0))
            part = self.rfile.read(n)
            self.calls.append((self.path, self.headers.get("Content-Type"), len(part)))
            q = dict(x.split("=") for x in self.path.split("?")[1].split("&"))
            FakeAp.tagfw["data"] = FakeAp.tagfw.get("data", b"")[:int(q["offset"])] + part
            if q["offset"] == "0":  # an AP from before "to" (the fake's old_ap) offers it to all
                to = q.get("to", "all")
                FakeAp.tagfw["to"] = "all" if to == "all" or FakeAp.tagfw.get("old_ap") else [to]
            if len(FakeAp.tagfw["data"]) < int(q["total"]):
                return self._send(200, {"received": len(FakeAp.tagfw["data"]), "total": int(q["total"])})
            return self._send(200, {"model": "1.54", "version": "0.3.0", "size": n, "crc": "00000000",
                                    "received": int(q["total"]), "total": int(q["total"])})
        if self.path.startswith("/api/tags/") and self.path.endswith("/draw"):
            body = self._body()
            mac = self.path.split("/")[3]
            self.calls.append((self.path, body))
            if getattr(FakeAp, "before_draw", False):  # an AP before 0.11.0
                return self._send(400, {"error": "mac must be 16 hex digits"})
            if mac not in self.tags:
                return self._send(404, {"error": "unknown tag"})
            return self._send(200, self._set(mac, {"layout": "image", "fields": [], "picture": len(json.dumps(body))}))
        if self.path.startswith("/api/tags/") and "/image" in self.path:
            n = int(self.headers.get("Content-Length", 0))
            data = self.rfile.read(n)
            mac = self.path.split("/")[3]
            self.calls.append((self.path, self.headers.get("Content-Type"), data))
            if mac not in self.tags:
                return self._send(409, {"error": "the tag's model is not known yet: it has not polled"})
            if data[:8] != esl_client.PNG_SIGNATURE and len(data) != 4736:
                return self._send(415, {"error": "not a PNG file (or the 4736 bytes of a raw image)"})
            r = self._set(mac, {"layout": "image", "fields": [], "picture": len(data)})
            FakeAp.images[mac] = data
            return self._send(200, r)
        body = self._body()
        self.calls.append((self.path, body))
        if self.path == "/api/time":
            FakeAp.time = body["time"]
            self._send(200, {"time": FakeAp.time, "source": "api"})
        elif self.path == "/api/config":
            if "poll_s" in body:
                FakeAp.config["poll_s"] = body["poll_s"] or 0
            if "webhook" in body:
                FakeAp.config["webhook"] = body["webhook"] or None
            if "tx_power" in body:
                FakeAp.config.update(tx_power=10 if body["tx_power"] is None else body["tx_power"], builtin_tx_power=10)
            if "channel" in body:
                ch = body["channel"] or 25
                now = body.get("channel_now") or ch == FakeAp.config.get("channel", 25)
                FakeAp.config.update(channel=ch if now else FakeAp.config.get("channel", 25), builtin_channel=25,
                                     channel_next=None if now else ch, channel_waiting=0 if now else 2, channel_due_s=780)
            if ("tz" in body or "ntp" in body) and "tz" not in FakeAp.config:
                return self._send(422, {"error": "this AP has no clock settings"})
            if "tz" in body:
                FakeAp.config["tz"] = body["tz"] or None
            if "ntp" in body:
                FakeAp.config["ntp"] = body["ntp"] or FakeAp.config["builtin_ntp"]
            if "lang" in body:
                if "lang" not in FakeAp.config:
                    return self._send(422, {"error": "this AP has no language setting"})
                FakeAp.config["lang"] = body["lang"] or "en"
            if ("ap_id" in body or "adopt" in body) and "ap_id" not in FakeAp.config:
                return self._send(422, {"error": "this AP has no AP id"})
            if "ap_id" in body:
                FakeAp.config["ap_id"] = body["ap_id"]
            if "adopt" in body:
                FakeAp.config["adopt"] = body["adopt"]
            self._send(200, FakeAp.config)
        elif self.path == "/api/wifi":
            FakeAp.wifi = {"mode": "sta", "ssid": body.get("ssid", FakeAp.wifi.get("ssid")) or None,
                           "connected": False, "setup_ssid": None,
                           "token_set": bool(body.get("token")) or FakeAp.wifi.get("token_set", False),
                           "hostname": body.get("hostname") or FakeAp.wifi.get("hostname", "openesl"),
                           "networks": []}
            self._send(200, FakeAp.wifi)
        elif self.path.startswith("/api/fonts/") and FakeAp.fonts is not None:
            if body.get("name") == "full":
                return self._send(507, {"error": "no room for the font file"})
            FakeAp.fonts[self.path.rsplit("/", 1)[1]] = body
            self._send(200, {"font": {"name": body["name"]}, "replaced": False, "redrawn": [],
                             "content_errors": [], "content_errors_total": 0})
        elif self.path.startswith("/api/layouts/"):
            model = self.path.rsplit("/", 1)[1]
            fonts = {f for x in body.get("layouts", []) for e in x.get("elements", []) for f in e.get("fonts", [])}
            if any(f not in ("small", "medium", "large", "mid", "text20", "price44") and f not in (FakeAp.fonts or {})
                   for f in fonts):
                self._send(422, {"error": "unknown font"})
                return
            if body.get("model") != model:
                self._send(422, {"error": "the file is for another model"})
                return
            FakeAp.layouts[model] = body
            self._send(200, {"model": model, "version": 1, "size": 1, "binary_size": 1,
                             "layouts": [x["name"] for x in body["layouts"]], "changed": True})
        elif self.path.endswith("/config") and self.path.startswith("/api/tags/"):
            mac = self.path.split("/")[3]
            FakeAp.tag_cfg[mac] = body
            if mac in self.tags:
                self.tags[mac]["config"] = self._cfg(mac)
            self._send(200, {"mac": mac, "poll_s": body.get("poll_s"), "config": self._cfg(mac),
                             "config_pending": True, "changed": True})
        elif self.path == "/api/tags":
            if len(body["tags"]) > esl_client.BATCH_MAX:
                self._send(413, {"error": "too many"})
                return
            if esl_client.json_tokens(body) > 1024:  # as the AP's JSON parser (ESL_API_MAX_TOKENS)
                self._send(413, {"error": "too many JSON tokens"})
                return
            bad = [i for i, r in enumerate(body["tags"]) if "BAD" in r.get("fields", [])]
            if bad:
                self._send(422, {"error": "layout \"BAD\" is not in any layout file", "index": bad[0]})
                return
            self._send(200, {"tags": [self._set(r["mac"], {k: v for k, v in r.items() if k != "mac"})
                                      for r in body["tags"]]})
        else:
            f = body["fields"]  # as an AP with 0.9.2 or older: text and one length byte per field share 96 bytes
            if len("".join(f).encode("latin-1", "replace")) + len(f) > 96:
                self._send(422, {"error": "text too long"})
                return
            self._send(200, self._set(self.path.rsplit("/", 1)[1], body))


class BarcodeTest(unittest.TestCase):
    def test_mac_from_barcode(self):
        # labels read on real tags, MACs from the flashers (tools/flash-tui)
        self.assertEqual(esl_client.mac_from_barcode("020663461013", "1.54"), "0000020663463415")
        self.assertEqual(esl_client.mac_from_barcode("020793500102", "1.54"), "000002079350341C")
        self.assertEqual(esl_client.mac_from_barcode("01c2 8092 0102", "2.9"), "000001C280923B15")
        with self.assertRaises(ValueError):
            esl_client.mac_from_barcode("0206634610", "1.54")

    def test_mac_command(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(esl_client.main(["mac", "020663461013", "--model", "1.54"]), 0)
        self.assertEqual(out.getvalue().split()[0], "0000020663463415")
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(esl_client.main(["mac", "020663461013"]), 0)
        self.assertEqual(len(out.getvalue().splitlines()), len(esl_client.MODEL_MAC_CODE))  # one per model
        with redirect_stderr(io.StringIO()):
            self.assertEqual(esl_client.main(["mac", "xyz"]), 2)


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
        FakeAp.tag_cfg.clear()
        FakeAp.layouts.clear()
        FakeAp.config = {"poll_s": 0, "builtin_poll_s": 0, "webhook": None}
        FakeAp.ota = {"slot": "ota_0", "version": "0.1.0", "slot_size": 1572864, "other_slot": "ota_1",
                      "other_version": None, "uploading": False}
        FakeAp.ota_rollback = False
        FakeAp.images = {}
        FakeAp.wifi = {}
        FakeAp.tagfw = {}
        FakeAp.ap_fw = "0.1.0"
        FakeAp.fonts = None
        FakeAp.other_aps = []

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
        self.assertIn("2950 mV", out)
        FakeAp.tags["00000123456789A8"]["battery_mv"] = 0     # 0 = the tag cannot measure it
        rc, out = self.run_cli("status")
        self.assertNotIn("0 mV", out)

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

        # an S3 AP whose C6 on the cable stopped answering: exit 1, and which radio
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = esl_client.print_health({"problems": [], "ok": False, "tags": 1, "with_problems": 0, "radio_lost": 1,
                                          "radio_problems": [{"radio": 0, "id": "000098880E7177C8", "via": "uart",
                                                              "problems": ["radio_lost"], "lost_s": 300}]})
        self.assertEqual(rc, 1)
        self.assertIn("  radio 0 (C6 on the cable, 000098880E7177C8) not answering for 5 min\n", buf.getvalue())

        rc, out = self.run_cli("config")
        self.assertEqual((rc, out), (0, "poll_s 0 (menuconfig 0), webhook off\n"))
        rc, out = self.run_cli("config", "--webhook", "http://10.0.0.5:8080/esl", "--poll-s", "120")
        self.assertEqual(rc, 0)
        self.assertEqual(FakeAp.calls[-1], ("/api/config", {"poll_s": 120, "webhook": "http://10.0.0.5:8080/esl"}))
        self.assertIn("poll_s 120 (menuconfig 0), webhook http://10.0.0.5:8080/esl", out)
        rc, out = self.run_cli("config", "--webhook", "", "--poll-s", "null")
        self.assertEqual(FakeAp.calls[-1], ("/api/config", {"poll_s": None, "webhook": None}))
        self.assertIn("webhook off", out)
        rc, out = self.run_cli("config", "--tx-power", "8")
        self.assertEqual(FakeAp.calls[-1], ("/api/config", {"tx_power": 8}))
        self.assertIn("webhook off, tx_power 8 dBm (menuconfig 10)", out)
        rc, out = self.run_cli("config", "--tx-power", "null")
        self.assertEqual(FakeAp.calls[-1], ("/api/config", {"tx_power": None}))
        self.assertIn("tx_power 10 dBm", out)
        rc, out = self.run_cli("config", "--channel", "20")
        self.assertEqual(FakeAp.calls[-1], ("/api/config", {"channel": 20}))
        self.assertIn("channel 25 (menuconfig 25), moving to 20: 2 tags have not confirmed it, at the latest in 780 s", out)
        rc, out = self.run_cli("config", "--channel", "20", "--channel-now")
        self.assertEqual(FakeAp.calls[-1], ("/api/config", {"channel": 20, "channel_now": True}))
        self.assertIn("channel 20 (menuconfig 25)", out)
        self.assertNotIn("moving", out)
        rc, out = self.run_cli("config", "--channel", "15", "--force")
        self.assertEqual(FakeAp.calls[-1], ("/api/config", {"channel": 15, "force": True}))

    def test_clock_config(self):
        FakeAp.config.update({"ntp": "pool.ntp.org", "builtin_ntp": "pool.ntp.org", "tz": None})
        rc, out = self.run_cli("config")
        self.assertIn("time zone UTC, NTP pool.ntp.org", out)
        rc, out = self.run_cli("config", "--tz", "CET-1CEST,M3.5.0,M10.5.0/3", "--ntp", "no.pool.ntp.org")
        self.assertEqual(FakeAp.calls[-1], ("/api/config", {"tz": "CET-1CEST,M3.5.0,M10.5.0/3", "ntp": "no.pool.ntp.org"}))
        self.assertIn("time zone CET-1CEST,M3.5.0,M10.5.0/3, NTP no.pool.ntp.org", out)
        rc, out = self.run_cli("config", "--tz", "", "--ntp", "")
        self.assertEqual(FakeAp.calls[-1], ("/api/config", {"tz": None, "ntp": None}))
        FakeAp.config["lang"] = "en"
        rc, out = self.run_cli("config", "--lang", "nb")
        self.assertEqual(FakeAp.calls[-1], ("/api/config", {"lang": "nb"}))
        self.assertIn("dates in Norwegian", out)

    def test_backup_and_restore_clock(self):
        FakeAp.config.update({"ntp": "no.pool.ntp.org", "builtin_ntp": "pool.ntp.org", "tz": "CET-1CEST,M3.5.0,M10.5.0/3"})
        cli = esl_client.Client(self.url, TOKEN)
        b = esl_client.backup(cli)
        self.assertEqual(b["config"], {"poll_s": 0, "webhook": None, "tz": "CET-1CEST,M3.5.0,M10.5.0/3",
                                       "ntp": "no.pool.ntp.org"})
        # onto an AP with clock settings: both are set
        self.setUp()
        FakeAp.config.update({"ntp": "pool.ntp.org", "builtin_ntp": "pool.ntp.org", "tz": None})
        lines, ok = esl_client.restore(cli, b)
        self.assertTrue(ok, lines)
        self.assertIn("config: poll_s 0, webhook off, time zone CET-1CEST,M3.5.0,M10.5.0/3, NTP no.pool.ntp.org", lines)
        self.assertEqual((FakeAp.config["tz"], FakeAp.config["ntp"]), ("CET-1CEST,M3.5.0,M10.5.0/3", "no.pool.ntp.org"))
        # onto an older AP: the rest goes on, with a note
        self.setUp()
        lines, ok = esl_client.restore(cli, b)
        self.assertTrue(ok, lines)
        self.assertIn("note: the time zone and NTP server were not restored; this AP's firmware has no clock settings", lines)
        # the AP's own NTP server is kept as None
        FakeAp.config.update({"ntp": "pool.ntp.org", "builtin_ntp": "pool.ntp.org", "tz": None})
        self.assertEqual(esl_client.backup(cli)["config"], {"poll_s": 0, "webhook": None, "tz": None, "ntp": None})
        # the date language (0.11.0): kept, and set again; an older AP gets a note
        FakeAp.config["lang"] = "nb"
        b = esl_client.backup(cli)
        self.assertEqual(b["config"]["lang"], "nb")
        self.setUp()
        FakeAp.config.update({"ntp": "pool.ntp.org", "builtin_ntp": "pool.ntp.org", "tz": None, "lang": "en"})
        lines, ok = esl_client.restore(cli, b)
        self.assertTrue(ok, lines)
        self.assertEqual(FakeAp.config["lang"], "nb")
        self.setUp()
        FakeAp.config.update({"ntp": "pool.ntp.org", "builtin_ntp": "pool.ntp.org", "tz": None})
        lines, ok = esl_client.restore(cli, b)
        self.assertTrue(ok, lines)
        self.assertIn("note: the date language was not restored; this AP's firmware has no language setting (0.11.0 and later)", lines)

    def test_backup_and_restore_ap_id(self):
        # #390: the id goes in the backup and back on a new AP, which then needs no adopt
        FakeAp.config.update({"ap_id": "1A2B3C4D", "adopt": None, "adopt_until": None})
        cli = esl_client.Client(self.url, TOKEN)
        b = esl_client.backup(cli)
        self.assertEqual(b["ap"]["ap_id"], "1A2B3C4D")
        self.setUp()
        FakeAp.config.update({"ap_id": "99990000", "adopt": "all", "adopt_until": None})
        lines, ok = esl_client.restore(cli, b)
        self.assertTrue(ok, lines)
        self.assertIn("AP id: 1A2B3C4D", lines)
        self.assertEqual((FakeAp.config["ap_id"], FakeAp.config["adopt"]), ("1A2B3C4D", None))
        # the AP the backup came from still answers its tags here: refused, unless --force
        self.setUp()
        FakeAp.config.update({"ap_id": "77778888", "adopt": None, "adopt_until": None})
        FakeAp.other_aps = [{"ap_id": "1A2B3C4D", "tags": 5, "polls": 40, "seen_s_ago": 30, "lost_long": False,
                             "answered_s_ago": 30}]
        lines, ok = esl_client.restore(cli, b)
        self.assertFalse(ok)
        self.assertEqual(FakeAp.config["ap_id"], "77778888")
        self.assertTrue(any("still answers its tags here" in line for line in lines), lines)
        # turned off a few minutes ago: its tags poll here without an answer (lost 1 and more), so it is taken
        FakeAp.other_aps[0].update({"answered_s_ago": 400})
        lines, ok = esl_client.restore(cli, b)
        self.assertTrue(ok, lines)
        self.assertEqual(FakeAp.config["ap_id"], "1A2B3C4D")
        FakeAp.config["ap_id"] = "77778888"
        FakeAp.other_aps[0].update({"answered_s_ago": 30})
        lines, ok = esl_client.restore(cli, b, force=True)
        self.assertEqual(FakeAp.config["ap_id"], "1A2B3C4D")
        # the same AP again: nothing sent, so an adopt that runs stays
        FakeAp.config["adopt"] = "BEEF0002"
        lines, ok = esl_client.restore(cli, b)
        self.assertEqual(FakeAp.config["adopt"], "BEEF0002")
        # a second AP with the same setup keeps its own
        self.setUp()
        FakeAp.config.update({"ap_id": "55556666", "adopt": None, "adopt_until": None})
        lines, ok = esl_client.restore(cli, b, copy=True)
        self.assertEqual(FakeAp.config["ap_id"], "55556666")
        self.assertTrue(any("keeps its own id 55556666" in line for line in lines), lines)
        # an AP without it: a note
        self.setUp()
        lines, ok = esl_client.restore(cli, b)
        self.assertTrue(ok, lines)
        self.assertTrue(any("has no AP id" in line for line in lines), lines)
        # config --adopt, and what config shows
        FakeAp.config.update({"ap_id": "1A2B3C4D", "adopt": None, "adopt_until": None})
        _, out = self.run_cli("config", "--adopt", "BEEF0002")
        self.assertEqual(FakeAp.calls[-1], ("/api/config", {"adopt": "BEEF0002"}))
        self.assertIn("AP id 1A2B3C4D, adopting the lost tags of AP BEEF0002", out)
        self.run_cli("config", "--adopt", "null")
        self.assertEqual(FakeAp.calls[-1], ("/api/config", {"adopt": None}))

    def test_log_and_report(self):
        self.run_cli("price", "00000123456789A8", "Milk", "24.90")
        FakeAp.tags["00000123456789A8"]["battery_mv"] = 2400       # low: listed by /api/health
        self.run_cli("price", "00000123456789B9", "Bread", "32.00")
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "log.csv")
            rc, out = self.run_cli("log", path, "--every", "1.1", "--rounds", "2")   # rounds are told apart by the second
            self.assertEqual(rc, 0)
            self.assertEqual(out.count("2 tags, 1 with problems, AP up 10 s"), 2)
            with open(path, newline="") as f:
                rows = list(csv.DictReader(f))
            self.assertEqual(len(rows), 4)
            self.assertEqual(rows[0]["mac"], "00000123456789A8")
            self.assertEqual(rows[0]["problems"], "low_battery")
            self.assertEqual(rows[1]["problems"], "")
            self.assertEqual(rows[0]["ap_uptime_s"], "10")
            # a third round appends without a second header
            time.sleep(1.1)
            rc, out = self.run_cli("log", path, "--every", "0", "--rounds", "1")
            with open(path) as f:
                self.assertEqual(f.read().count("time,ap_uptime_s"), 1)
            # a round the AP did not answer, and an AP restart, hand-written as "log" writes them (dated after the real rounds)
            with open(path, "a", newline="") as f:
                w = csv.writer(f)
                w.writerow(["2099-01-01 12:00:00", "", "no answer", "", "", "", "", "", "", "", "", "", "timeout"])
                w.writerow(["2099-01-01 12:01:00", "5", "task_wdt", "00000123456789A8", "2.9", "700", "2400", "-60",
                            "1", "1", "0", "1", "low_battery|silent"])
            rc, out = self.run_cli("report", path)
        self.assertEqual(rc, 1)
        self.assertIn("4 rounds", out)
        self.assertIn("AP did not answer 1 times, restarted 1 times (2099-01-01 12:01:00)", out)
        a8 = next(l for l in out.splitlines() if l.startswith("00000123456789A8"))
        self.assertIn("11 min", a8)          # the longest gap, 700 s
        self.assertIn("2400 mV", a8)
        self.assertIn("low_battery|silent", a8)
        b9 = next(l for l in out.splitlines() if l.startswith("00000123456789B9"))
        self.assertIn("     3 ", b9)         # 3 rounds, no problems

    def test_draw(self):
        lay = {"name": "room", "fields": ["room", "next"],
               "elements": [{"type": "ellipse", "x": 0, "y": 0, "w": 20, "h": 20}]}
        # draw_body: a layout file (by name, with its icons), one layout, a draw body; --field by name
        f = {"format": "esl-layouts", "version": 1, "model": "2.9", "icons": {"i": {"w": 8, "h": 1, "data": "/w=="}},
             "layouts": [lay, dict(lay, name="other")]}
        self.assertEqual(esl_client.draw_body(f, "room"), {"layout": lay, "icons": f["icons"]})
        with self.assertRaisesRegex(ValueError, "--layout"):
            esl_client.draw_body(f)
        with self.assertRaisesRegex(ValueError, "no layout x"):
            esl_client.draw_body(f, "x")
        self.assertEqual(esl_client.draw_body(lay, None, {"room": "R2"}), {"layout": lay, "fields": {"room": "R2"}})
        body = {"layout": lay, "fields": ["R1", "Free"]}
        self.assertEqual(esl_client.draw_body(body, None, {"next": "Busy"})["fields"], {"room": "R1", "next": "Busy"})
        self.assertEqual(esl_client.draw_body(body), body)
        with self.assertRaisesRegex(ValueError, "list of layouts"):
            esl_client.draw_body({"layouts": ["room"]})

        self.run_cli("text", "00000123456789A8", "x")
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as tf:
            json.dump(f, tf)
        try:
            rc, out = self.run_cli("draw", "00000123456789A8", tf.name, "--layout", "room", "--field", "room=Rom 2")
            self.assertEqual(rc, 0, out)
            self.assertIn("changed", out)
            self.assertEqual(FakeAp.calls[-1], ("/api/tags/00000123456789A8/draw",
                                                {"layout": lay, "icons": f["icons"], "fields": {"room": "Rom 2"}}))
            rc, out = self.run_cli("draw", "00000123456789A8", tf.name)
            self.assertEqual(rc, 2)
            rc, out = self.run_cli("draw", "00000123456789A8", tf.name, "--layout", "room", "--field", "room")
            self.assertEqual(rc, 2)
            rc, out = self.run_cli("draw", "00000123456789FF", tf.name, "--layout", "room")
            self.assertEqual(rc, 2)
            FakeAp.before_draw = True
            try:
                rc, out = self.run_cli("draw", "00000123456789A8", tf.name, "--layout", "room")
                self.assertEqual(rc, 2)
                with self.assertRaisesRegex(esl_client.ApiError, "older than 0.11.0"):
                    esl_client.Client(self.url, TOKEN).draw("00000123456789A8", {"layout": lay})
            finally:
                FakeAp.before_draw = False
        finally:
            os.unlink(tf.name)

    def test_image(self):
        self.run_cli("text", "00000123456789A8", "x")
        png = esl_client.PNG_SIGNATURE + b"rest of the file"
        with tempfile.NamedTemporaryFile(suffix=".png") as f:
            f.write(png)
            f.flush()
            rc, out = self.run_cli("image", "00000123456789A8", f.name, "--dither")
            self.assertEqual(rc, 0, out)
            self.assertIn("changed", out)
            self.assertEqual(FakeAp.calls[-1], ("/api/tags/00000123456789A8/image?dither=true", "image/png", png))
            rc, out = self.run_cli("image", "00000123456789A9", f.name)
            self.assertEqual(rc, 2)
        with tempfile.NamedTemporaryFile(suffix=".bin") as f:
            f.write(bytes(4736))
            f.flush()
            rc, out = self.run_cli("image", "00000123456789A8", f.name)
            self.assertEqual(rc, 0, out)
            self.assertEqual(FakeAp.calls[-1][:2], ("/api/tags/00000123456789A8/image", "application/octet-stream"))

    def test_battery(self):
        # a 2.9" tag read every day for 30 days: 2600 (2.6 V or more) for 10 days, 2550 from day 10,
        # 2450 from day 20 (a bounce back to 2550 on day 25 must not count as a new step), and three
        # screen updates; a 1.54" tag that reports 0; a round the AP did not answer
        def row(day, mac, model, mv, shown):
            return [f"2026-11-{day:02d} 08:00:00", str(day * 86400), "", mac, model, "30", mv, "-60",
                    shown, shown, "0", "0", ""]
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "log.csv")
            with open(path, "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["time", "ap_uptime_s", "ap_reset", "mac", "model", "seen_s_ago", "battery_mv", "rssi",
                            "content_id", "shown_content_id", "pending", "draw_failed", "problems"])
                for day in range(1, 31):
                    mv = "2600" if day <= 10 else "2550" if day <= 20 or day == 25 else "2450"
                    w.writerow(row(day, "00000123456789A8", "2.9", mv, "1" if day < 5 else "2" if day < 15 else "3"))
                    w.writerow(row(day, "00000123456789B9", "1.54", "0", "7"))
                w.writerow(["2026-11-30 09:00:00", "", "no answer", "", "", "", "", "", "", "", "", "", "timeout"])
            out_csv = os.path.join(d, "batt.csv")
            out_html = os.path.join(d, "batt.html")
            rc, out = self.run_cli("battery", path, "--csv", out_csv, "--low-mv", "2400", "--html", out_html)
            with open(out_csv, newline="") as f:
                table = {r["mac"]: r for r in csv.DictReader(f)}
            with open(out_html, encoding="utf-8") as f:
                html = f.read()
        self.assertEqual(rc, 0)
        self.assertIn("2600 mV since the log started, 2550 mV from 2026-11-11 08:00, 2450 mV from 2026-11-21 08:00", out)
        self.assertIn("falling 10.0 mV/day", out)          # 100 mV in the 10 days between the two steps
        self.assertIn("about 5 days until 2400 mV", out)   # (2450 - 2400) / 10
        self.assertIn("no battery reading (0 = the tag cannot measure it)", out)
        a8 = table["00000123456789A8"]
        self.assertEqual((a8["first_mv"], a8["last_mv"], a8["min_mv"], a8["steps"], a8["draws"]), ("2600", "2450", "2450", "2", "2"))
        self.assertEqual(a8["days"], "29.0")
        self.assertEqual(a8["polls"], str(29 * 1440))
        self.assertEqual(table["00000123456789B9"]["first_mv"], "")
        self.assertEqual(html.count("<svg"), 1)                  # a chart for the 2.9" only
        self.assertEqual(html.count("<circle"), 3)               # its three steps
        self.assertEqual(html.count("<polygon"), 2)              # its two screen updates
        self.assertIn("about 5.0 days to low", html)
        self.assertIn("<td>00000123456789B9</td>", html)         # the 1.54" is in the table
        # a log that starts below 2600: the first reading has no known time, so one more step gives no rate
        # (the tag may have sat at 2500 for weeks); the second does, and "already below" shows up in the HTML
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "log.csv")
            with open(path, "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["time", "ap_uptime_s", "ap_reset", "mac", "model", "seen_s_ago", "battery_mv", "rssi",
                            "content_id", "shown_content_id", "pending", "draw_failed", "problems"])
                for day in range(1, 31):
                    mv = "2550" if day <= 10 else "2450" if day <= 20 else "2350"
                    w.writerow(row(day, "00000123456789A8", "" if day == 1 else "2.9", mv, "1"))
            rc, out = self.run_cli("battery", path, "--low-mv", "2450", "--html", os.path.join(d, "b.html"))
            with open(os.path.join(d, "b.html"), encoding="utf-8") as f:
                html2 = f.read()
        self.assertIn("2550 mV since the log started, 2450 mV from 2026-11-11 08:00, 2350 mV from 2026-11-21 08:00", out)
        self.assertIn("falling 10.0 mV/day", out)                # from the two steps seen, not from the log start
        self.assertIn("already below 2450 mV", out)
        self.assertIn("already below the low line", html2)
        self.assertIn("<td>2.9</td>", html2)                     # the model from the first row that has one

    LOG_HEADER = ["time", "ap_uptime_s", "ap_reset", "mac", "model", "seen_s_ago", "battery_mv", "rssi",
                  "content_id", "shown_content_id", "pending", "draw_failed", "problems"]

    def write_log(self, d, rows, header=None):
        path = os.path.join(d, "log.csv")
        with open(path, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(header or self.LOG_HEADER)
            w.writerows(rows)
        return path

    def test_battery_figures_match_power_budget(self):
        # the charge count must use the same figures as tools/power-budget, or the two disagree
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "power_budget", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "power-budget", "power_budget.py"))
        pb = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(pb)
        p = pb.defaults()
        self.assertAlmostEqual(esl_client.BATT_POLL_MC, pb.per_poll_mc(p, True), places=2)
        self.assertAlmostEqual(esl_client.BATT_DRAW_MC, pb.per_update_mc(p), delta=0.5)
        self.assertEqual(esl_client.BATT_SLEEP_UA, p["sleep_ua"])
        self.assertEqual(esl_client.BATT_CELL_MAH, p["cell_mah"])

    def test_battery_charge_count(self):
        # one 2.9" tag over exactly one day at 60 s with one screen update:
        # 1440 polls * 0.08 + 22 + 3 uA * 86400 s = 396.4 mC = 0.110 mAh, 4.59 uA on average
        rows = [["2026-11-01 08:00:00", "1", "", "00000123456789A8", "2.9", "30", "2600", "-60", "1", "1", "0", "0", ""],
                ["2026-11-02 08:00:00", "2", "", "00000123456789A8", "2.9", "30", "2600", "-60", "2", "2", "0", "0", ""]]
        with tempfile.TemporaryDirectory() as d:
            lines, table, series = esl_client.battery_analysis(esl_client.read_log(self.write_log(d, rows)))
        row = table[0]
        self.assertEqual((row["polls"], row["draws"], row["charge_mah"], row["avg_ua"]), (1440, 1, 0.11, 4.59))
        # 2 cells * 620 mAh * 0.7, with 1 % a year self-discharge on top of the average current
        self.assertEqual(row["years_at_this_rate"], 16.5)
        self.assertIsNone(row["mv_per_day"])
        self.assertIn("2.6 V or more for the whole log", "\n".join(lines))
        # --poll-s changes the poll count
        _, table, _ = esl_client.battery_analysis([dict(zip(self.LOG_HEADER, r)) for r in rows], poll_s=300)
        self.assertEqual(table[0]["polls"], 288)

    def test_battery_edge_cases(self):
        with tempfile.TemporaryDirectory() as d:
            # no tags: only rounds the AP did not answer
            path = self.write_log(d, [["2026-11-01 08:00:00", "", "no answer", "", "", "", "", "", "", "", "", "", "timeout"]])
            rc, out = self.run_cli("battery", path)
            self.assertEqual(rc, 0)
            self.assertIn("no tags in the log", out)
            # a single round: no time span, so no average and no years, and nothing divides by zero
            path = self.write_log(d, [["2026-11-01 08:00:00", "1", "", "00000123456789A8", "2.9", "30", "2450", "-60",
                                       "1", "1", "0", "0", ""]])
            rc, out = self.run_cli("battery", path, "--html", os.path.join(d, "one.html"))
            self.assertEqual(rc, 0, out)
            self.assertIn("2450 mV since the log started", out)
            self.assertIn("the rate needs two steps seen in the log", out)
            # an older log without the battery and content columns still reads
            path = self.write_log(d, [["2026-11-01 08:00:00", "00000123456789A8", "2.9"],
                                      ["2026-11-03 08:00:00", "00000123456789A8", "2.9"]], header=["time", "mac", "model"])
            rc, out = self.run_cli("battery", path)
            self.assertEqual(rc, 0, out)
            self.assertIn("2.0 days in the log", out)
            self.assertIn("no battery reading", out)
            # errors: a missing file, a CSV that is not a log, a bad time stamp
            err = io.StringIO()
            with redirect_stderr(err):
                rc, _ = self.run_cli("battery", os.path.join(d, "missing.csv"))
                self.assertEqual(rc, 2)
                rc, _ = self.run_cli("battery", self.write_log(d, [["1", "2"]], header=["a", "b"]))
                self.assertEqual(rc, 2)
                rc, _ = self.run_cli("battery", self.write_log(d, [["yesterday", "00000123456789A8"]], header=["time", "mac"]))
                self.assertEqual(rc, 2)
            self.assertIn("not a time from esl_client.py log: 'yesterday'", err.getvalue())

    def test_battery_long_log(self):
        # a row a minute for 4 days: 2600 for a day, then 2550, a bounce to 2600 for an hour, 2450 from
        # day 3. Only the change points are kept, and the result is the same as for a log with one row
        # per change (apart from the time span, which both have)
        start = datetime.datetime(2026, 11, 1, 8, 0, 0)
        def mv_at(m):
            if m < 1440:
                return "2600"
            if 2000 <= m < 2060:
                return "2600"
            return "2550" if m < 2 * 1440 else "2450"
        full, changes, last = [], [], None
        for m in range(4 * 1440 + 1):
            r = [(start + datetime.timedelta(minutes=m)).strftime("%Y-%m-%d %H:%M:%S"), str(m * 60), "",
                 "00000123456789A8", "2.9", "30", mv_at(m), "-60", "1", "1", "0", "0", ""]
            full.append(r)
            if r[6] != last or m == 4 * 1440:
                changes.append(r)
                last = r[6]
        with tempfile.TemporaryDirectory() as d:
            rows = esl_client.iter_log(self.write_log(d, full))
            self.assertNotIsInstance(rows, list)              # read lazily, not all into memory
            lines_full, table_full, series_full = esl_client.battery_analysis(rows)
        lines_ch, table_ch, series_ch = esl_client.battery_analysis([dict(zip(self.LOG_HEADER, r)) for r in changes])
        self.assertEqual(len(series_full["00000123456789A8"]["mv"]), 5)   # 2600, 2550, 2600, 2550, 2450
        self.assertEqual(series_full["00000123456789A8"]["mv"], series_ch["00000123456789A8"]["mv"])
        self.assertEqual(table_full[0]["steps"], 2)
        self.assertEqual(table_full[0]["min_mv"], 2450)
        self.assertEqual({k: v for k, v in table_full[0].items()}, {k: v for k, v in table_ch[0].items()})
        self.assertEqual(lines_full[1:], lines_ch[1:])     # the first line counts the rounds

    def test_battery_poll_s(self):
        with tempfile.TemporaryDirectory() as d:
            path = self.write_log(d, [["2026-11-01 08:00:00", "00000123456789A8", "2.9"]], header=["time", "mac", "model"])
            for bad in ("0", "-60"):
                err = io.StringIO()
                with redirect_stderr(err):
                    rc, _ = self.run_cli("battery", path, "--poll-s", bad)
                self.assertEqual(rc, 2)
                self.assertIn("--poll-s must be 1 or more", err.getvalue())

    def test_battery_html_escapes(self):
        # the page is built from the log's text: a hostile model or MAC must not become markup
        rows = [{"time": "2026-11-01 08:00:00", "mac": "<script>x</script>", "model": "\"><img src=x>",
                 "battery_mv": "2550", "ap_reset": "", "shown_content_id": "1"},
                {"time": "2026-11-02 08:00:00", "mac": "<script>x</script>", "model": "2.9",
                 "battery_mv": "2450", "ap_reset": "", "shown_content_id": "1"}]
        _, table, series = esl_client.battery_analysis(rows)
        page = esl_client.battery_html(table, series)
        self.assertNotIn("<script>", page)
        self.assertNotIn("<img", page)
        self.assertIn("&lt;script&gt;x&lt;/script&gt;", page)
        self.assertIn("aria-label=\"battery of &lt;script&gt;", page)

    def test_backup_and_restore(self):
        layout = {"format": "esl-layouts", "version": 1, "model": "2.9",
                  "layouts": [{"name": "shelf", "fields": ["a"], "elements": [{"type": "text", "field": "a",
                                                                                "x": 0, "y": 0, "w": 10, "h": 20}]}]}
        FakeAp.layouts["2.9"] = layout
        FakeAp.config.update({"poll_s": 120, "webhook": "http://10.0.0.5:8080/esl"})
        self.run_cli("price", "00000123456789A8", "Milk", "24.90", "--highlight")
        self.run_cli("text", "00000123456789A9", "Open", "9-21", "--frame")
        FakeAp.tag_cfg["00000123456789A9"] = {"poll_s": 300}
        FakeAp.tags["00000123456789A9"]["config"] = FakeAp._cfg("00000123456789A9")
        FakeAp.tags["00000123456789B0"] = {"mac": "00000123456789B0", "content_id": 0, "shown_content_id": 0,
                                           "pending": False, "model": "2.9", "seen_s_ago": 3, "battery_mv": 2950,
                                           "rssi": -60, "config": FakeAp._cfg("00000123456789B0")}
        raw = bytes(range(256)) * 18 + bytes(128)
        FakeAp.tags["00000123456789B1"] = dict(FakeAp.tags["00000123456789B0"], mac="00000123456789B1")
        self.assertEqual(esl_client.Client(self.url, TOKEN).set_image("00000123456789B1", raw)["changed"], True)
        FakeAp.wifi = {"mode": "sta", "ssid": "Butikken", "connected": True, "hostname": "butikk"}
        future = int(time.time()) + 3600
        FakeAp.sched = [{"mac": "00000123456789A8", "at": future, "layout": "price", "fields": ["Milk", "19.90"]},
                        {"mac": "00000123456789A8", "at": 1700000000, "layout": "price", "fields": ["Old", "1"]}]
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "ap.json")
            rc, out = self.run_cli("backup", path)
            self.assertEqual((rc, out), (0, f"{path}: 3 tags, 1 layout files, 2 scheduled\n"))
            with open(path, encoding="utf-8") as f:
                b = json.load(f)
            self.assertEqual(b["format"], "openesl-backup")
            self.assertEqual(b["config"], {"poll_s": 120, "webhook": "http://10.0.0.5:8080/esl"})
            self.assertEqual(b["layouts"], {"2.9": layout})
            self.assertEqual(b["tags"], [
                {"mac": "00000123456789A8", "layout": "price", "fields": ["Milk", "24.90"], "price_highlight": True,
                 "frame": False},
                {"mac": "00000123456789A9", "layout": "text", "fields": ["Open", "9-21"], "price_highlight": False,
                 "frame": True, "config": {"poll_s": 300}},
                {"mac": "00000123456789B1", "image": base64.b64encode(raw).decode()}])
            self.assertEqual([e["at"] for e in b["scheduled"]], [1700000000, future])
            self.assertEqual(b["ap"]["hostname"], "butikk")
            self.assertEqual(b["tag_firmware"], [{"model": "1.54", "version": "0.3.0", "crc": "1234ABCD", "to": "all",
                                                  "waiting": 1}])

            # A fresh AP (as after erase-flash): everything comes back, except the entry whose time has passed.
            # Its name and the tag firmware are not set; restore says how.
            self.setUp()
            FakeAp.wifi = {"mode": "setup", "ssid": None, "connected": False, "hostname": "openesl-77c8"}
            FakeAp.tagfw = {"images": [{"model": "1.54", "version": None}]}
            rc, out = self.run_cli("restore", path, "--dry-run")
            self.assertEqual(rc, 0)
            self.assertIn("note: the old AP offered 1.54 tag firmware 0.3.0 to all its tags (1 not updated yet); send "
                          "it again with tag-ota openesl-tag-1.54-v<release>-ota.bin --all, from the release whose 1.54 "
                          "firmware is 0.3.0", out)
            self.assertIn("note: the old AP was called butikk (butikk.local), this one openesl-77c8; to keep the "
                          "old name: wifi --hostname butikk", out)
            self.assertIn("tags: 2 with content, 1 with a picture, 1 with their own settings", out)
            self.assertIn("scheduled: 1 (1 skipped, their time has passed)", out)
            self.assertIn("nothing sent", out)
            self.assertEqual(FakeAp.calls, [])
            # the tag with the picture has not polled the fresh AP yet: it gets it the next time
            rc, out = self.run_cli("restore", path)
            self.assertEqual(rc, 1, out)
            self.assertIn("warning: 1 pictures wait until their tags have polled this AP", out)
            self.assertIn("restored: 1 layout files, 1 tag settings, 2 tags, 1 scheduled", out)
            FakeAp.tags["00000123456789B1"] = dict(FakeAp.tags["00000123456789A8"], mac="00000123456789B1")
            FakeAp.sched = []  # the fake keeps each entry sent, the AP the last per time
            rc, out = self.run_cli("restore", path)
            self.assertEqual(rc, 0, out)
            self.assertIn("restored: 1 layout files, 1 tag settings, 3 tags, 1 scheduled", out)
            self.assertEqual(FakeAp.images["00000123456789B1"], raw)
            self.assertEqual(FakeAp.config["poll_s"], 120)
            self.assertEqual(FakeAp.config["webhook"], "http://10.0.0.5:8080/esl")
            self.assertEqual(FakeAp.layouts, {"2.9": layout})
            self.assertEqual(FakeAp.tag_cfg, {"00000123456789A9": {"poll_s": 300}})
            self.assertEqual(sorted(FakeAp.tags), ["00000123456789A8", "00000123456789A9", "00000123456789B1"])
            self.assertEqual(FakeAp.tags["00000123456789A8"]["fields"], ["Milk", "24.90"])
            self.assertTrue(FakeAp.tags["00000123456789A8"]["price_highlight"])
            self.assertEqual(FakeAp.sched, [{"mac": "00000123456789A8", "at": future, "layout": "price",
                                             "fields": ["Milk", "19.90"]}])
            self.assertIsNotNone(FakeAp.time)  # set before the scheduled entry was sent
            paths = [c[0] for c in FakeAp.calls]
            self.assertLess(paths.index("/api/layouts/2.9"), paths.index("/api/tags"))
            self.assertLess(paths.index("/api/config"), paths.index("/api/layouts/2.9"))

            self.assertEqual(out.count("note: "), 2)

            # The same backup again changes nothing; with the name and firmware set there is nothing to note
            FakeAp.wifi["hostname"] = "butikk"
            del FakeAp.tagfw["images"]
            rc, out = self.run_cli("restore", path)
            self.assertEqual(rc, 0)
            self.assertNotIn("note: ", out)

            # The tag firmware note: none when this AP offers the model already (a newer version must not be
            # taken back) or no tag was waiting; for chosen tags it names them
            cli = esl_client.Client(self.url, TOKEN)
            FakeAp.tagfw = {"images": [{"model": "1.54", "version": "0.4.0", "crc": "0000FFFF", "to": "all"}]}
            self.assertEqual(esl_client.restore_notes(cli, b), [])
            FakeAp.tagfw = {"images": []}
            self.assertEqual(esl_client.restore_notes(cli, dict(b, tag_firmware=[dict(b["tag_firmware"][0],
                                                                                       waiting=0)])), [])
            notes = esl_client.restore_notes(cli, dict(b, tag_firmware=[dict(b["tag_firmware"][0],
                                                                             to=["00000123456789A8"])]))
            self.assertEqual(len(notes), 1)
            self.assertIn("to 1 chosen tag (1 not updated yet); send it again to the same tags with tag-ota "
                          "openesl-tag-1.54-v<release>-ota.bin --to 00000123456789A8, from the release", notes[0])

            # A broken "ap" or "tag_firmware" is refused before anything is sent
            for key, bad in (("ap", "x"), ("tag_firmware", {})):
                with open(path, "w") as f:
                    json.dump(dict(b, **{key: bad}), f)
                with self.assertRaises(esl_client.BackupError):
                    esl_client.read_backup(path)
            with open(path, "w") as f:
                json.dump(b, f)

            # An older backup, without the name and tag firmware, restores as before
            with open(path, encoding="utf-8") as f:
                b = json.load(f)
            del b["tag_firmware"], b["ap"]["hostname"]
            lines, ok = esl_client.restore(esl_client.Client(self.url, TOKEN), b, True)
            self.assertTrue(ok)
            self.assertFalse([x for x in lines if x.startswith("note: ")])

            with open(path, "w") as f:
                f.write("{\"format\": \"something\"}")
            rc, out = self.run_cli("restore", path)
            self.assertEqual(rc, 1)

    def test_backup_and_restore_font_files(self):
        # A layout file that draws with a font file (#177): the font goes in the backup and back first
        font = {"format": "esl-font", "version": 1, "name": "text28", "height": 10, "baseline": 8,
                "first": 65, "last": 65, "widths": [1], "bitmap": "/8A="}
        layout = {"format": "esl-layouts", "version": 1, "model": "2.9",
                  "layouts": [{"name": "s", "fields": ["a"], "elements": [{"type": "text", "field": "a", "x": 0,
                                                                            "y": 0, "w": 10, "h": 20,
                                                                            "fonts": ["text28"]}]}]}
        FakeAp.fonts = {"text28": font}
        FakeAp.layouts["2.9"] = layout
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "ap.json")
            self.assertEqual(self.run_cli("backup", path)[0], 0)
            with open(path, encoding="utf-8") as f:
                b = json.load(f)
            self.assertEqual(b["fonts"], {"text28": font})
            self.setUp()
            FakeAp.fonts = {}
            rc, out = self.run_cli("restore", path)
            self.assertEqual(rc, 0, out)
            self.assertIn("font files: text28", out)
            self.assertEqual((FakeAp.fonts, FakeAp.layouts), ({"text28": font}, {"2.9": layout}))
            paths = [c[0] for c in FakeAp.calls]
            self.assertLess(paths.index("/api/fonts/text28"), paths.index("/api/layouts/2.9"))
            # a font the AP turns down is a warning; the rest is still restored
            b["fonts"] = {"full": dict(font, name="full"), **b["fonts"]}
            with open(path, "w", encoding="utf-8") as f:
                json.dump(b, f)
            self.setUp()
            FakeAp.fonts = {}
            rc, out = self.run_cli("restore", path)
            self.assertEqual(rc, 1, out)
            self.assertIn("warning: font file full was not restored (HTTP 507", out)
            self.assertEqual(FakeAp.layouts, {"2.9": layout})
            # an AP without font files has no "fonts" in its backup, as before
            self.setUp()
            FakeAp.layouts["2.9"] = dict(layout, layouts=[])
            self.run_cli("backup", path)
            with open(path, encoding="utf-8") as f:
                self.assertNotIn("fonts", json.load(f))

    def test_restore_backup_from_091(self):
        # 0.9.0 and 0.9.1 put the radio key (and "only paired tags") in the backup; this AP
        # has neither, so they are left out and the rest is restored
        cli = esl_client.Client(self.url, TOKEN)
        b = {"format": "openesl-backup", "version": 1, "taken": "2026-10-30T12:00:00+00:00",
             "radio_key": "00112233445566778899AABBCCDDEEFF",
             "config": {"poll_s": 120, "webhook": None, "radio_secure": True}, "layouts": {},
             "tags": [{"mac": "00000123456789A8", "layout": "price", "fields": ["Milk", "24.90"]}], "scheduled": []}
        lines, ok = esl_client.restore(cli, b)
        self.assertTrue(ok, lines)
        self.assertEqual(FakeAp.config["poll_s"], 120)
        self.assertIn("00000123456789A8", FakeAp.tags)
        self.assertNotIn("radio_secure", FakeAp.calls[0][1])
        # the 0.9.x web page's radio key file has only the key: the AP keeps its own settings
        self.setUp()
        FakeAp.config["poll_s"] = 600
        lines, ok = esl_client.restore(cli, dict(b, config={}, tags=[]))
        self.assertTrue(ok, lines)
        self.assertEqual(FakeAp.config["poll_s"], 600)
        self.assertNotIn("/api/config", [c[0] for c in FakeAp.calls])

    def test_backup_and_restore_picture_errors(self):
        self.run_cli("text", "00000123456789A8", "x")
        cli = esl_client.Client(self.url, TOKEN)
        cli.set_image("00000123456789A8", bytes(4736))
        # the AP cannot give the picture back: the tag is left out, with a warning
        del FakeAp.images["00000123456789A8"]
        warnings = []
        b = esl_client.backup(cli, warn=warnings.append)
        self.assertEqual(b["tags"], [])
        self.assertEqual(len(warnings), 1)
        self.assertIn("00000123456789A8: its picture was left out (HTTP 404", warnings[0])
        # a picture the AP refuses stops the restore, saying how far it got
        b["tags"] = [{"mac": "00000123456789A8", "image": base64.b64encode(bytes(100)).decode()}]
        lines, ok = esl_client.restore(cli, b, False)
        self.assertFalse(ok)
        self.assertIn("error: 00000123456789A8: HTTP 415", lines[-1])
        self.assertIn("0 tags were restored before it", lines[-1])

    def test_restore_backup_from_releases(self):
        """#307: what "backup" of every release since 0.8.2 wrote (compat/, make_backup.py) still restores."""
        files = sorted(glob.glob(os.path.join(os.path.dirname(os.path.abspath(__file__)), "compat", "v*-backup.json")))
        self.assertTrue(files)
        for path in files:
            with self.subTest(os.path.basename(path)):
                self.setUp()  # a fresh AP, as after erase-flash; the tag with the picture has polled it
                FakeAp.tags["00000123456789B1"] = {"mac": "00000123456789B1", "content_id": 0, "model": "2.9"}
                rc, out = self.run_cli("restore", path)
                self.assertEqual(rc, 0, out)
                self.assertIn("restored: 1 layout files, 1 tag settings, 4 tags, 1 scheduled", out)
                self.assertEqual(FakeAp.config["poll_s"], 120)
                self.assertEqual(FakeAp.layouts["2.9"]["layouts"][0]["name"], "shelf")
                self.assertEqual(FakeAp.tag_cfg, {"00000123456789A9": {"poll_s": 300}})
                self.assertEqual(FakeAp.tags["00000123456789A8"]["fields"], ["Milk 1 L", "24.90"])
                self.assertTrue(FakeAp.tags["00000123456789A8"]["price_highlight"])
                self.assertTrue(FakeAp.tags["00000123456789A9"]["frame"])
                self.assertEqual(FakeAp.tags["00000123456789AA"]["layout"], "shelf")
                self.assertEqual(FakeAp.images["00000123456789B1"], bytes(range(256)) * 18 + bytes(128))
                self.assertEqual([(e["mac"], e["at"]) for e in FakeAp.sched], [("00000123456789A8", 4102444800)])

    def test_restore_dashboard_backups_from_releases(self):
        """#307: what the dashboard of every release since 0.9.0 wrote (web/test/compat/,
        make_backup.mjs) restores with esl-client too: it is the same format."""
        here = os.path.dirname(os.path.abspath(__file__))
        files = sorted(glob.glob(os.path.join(here, "..", "..", "web", "test", "compat", "v*-dashboard-backup.json")))
        self.assertTrue(files)
        for path in files:
            with open(path, encoding="utf-8") as f:
                b = json.load(f)
            with self.subTest(os.path.basename(path)):
                self.setUp()
                FakeAp.fonts = {}
                for t in b["tags"]:
                    if "image" in t:  # the tag with the picture has polled
                        FakeAp.tags[t["mac"]] = {"mac": t["mac"], "content_id": 0, "model": "2.9"}
                rc, out = self.run_cli("restore", path)
                self.assertEqual(rc, 0, out)
                content = [t for t in b["tags"] if "layout" in t]
                self.assertIn(f"restored: {len(b['layouts'])} layout files, 1 tag settings, {len(content) + 1} tags, "
                              f"{len(b['scheduled'])} scheduled", out)
                self.assertEqual(FakeAp.config["poll_s"], b["config"]["poll_s"])
                self.assertEqual(FakeAp.fonts, b.get("fonts", {}))
                self.assertEqual(FakeAp.layouts, b["layouts"])
                self.assertEqual(FakeAp.tag_cfg, {t["mac"]: t["config"] for t in b["tags"] if "config" in t})
                for t in content:
                    for k in ("layout", "fields", "price_highlight", "frame"):
                        if k in t:
                            self.assertEqual(FakeAp.tags[t["mac"]][k], t[k], (t["mac"], k))
                for t in b["tags"]:
                    if "image" in t:
                        self.assertEqual(FakeAp.images[t["mac"]], base64.b64decode(t["image"]))
                self.assertEqual([(e["mac"], e["at"]) for e in FakeAp.sched], [(e["mac"], e["at"]) for e in b["scheduled"]])

    def test_tag_ota(self):
        rc, out = self.run_cli("tag-ota")
        self.assertEqual(rc, 0)
        self.assertEqual(out, "  2.9: no firmware\n"
                              " 1.54: 0.3.0 (53248 bytes, CRC 1234ABCD) for all tags: 2 updated, 1 pending, 0 failed, "
                              "1 without OTA\n")
        data = b"OSFW" + bytes([1, 2]) + (30000).to_bytes(2, "little") + bytes(8) + bytes(range(256)) * 160
        FakeAp.tagfw = {}
        with tempfile.NamedTemporaryFile(suffix=".bin") as f:
            f.write(data)
            f.flush()
            # without saying who gets it, nothing is sent: one tag first, or --all
            err = io.StringIO()
            with redirect_stderr(err):
                rc, out = self.run_cli("tag-ota", f.name)
            self.assertEqual(rc, 2)
            self.assertIn("say which 1.54 tags get it: --to MAC (one tag first", err.getvalue())
            self.assertEqual(FakeAp.calls, [])
            rc, out = self.run_cli("tag-ota", f.name, "--all")
        self.assertEqual(rc, 0)
        self.assertIn("sending 1.54 firmware 3.0.0 (40976 bytes) to every 1.54 tag", out)
        self.assertIn("the AP offers 1.54 firmware 0.3.0;", out)
        self.assertIn(" 1.54: 0.3.0 (53248 bytes, CRC 1234ABCD) for all tags:", out)
        self.assertEqual(FakeAp.tagfw["data"], data)
        parts = [c for c in FakeAp.calls if c[0].startswith("/api/tag-firmware?")]
        self.assertEqual([c[0] for c in parts], [f"/api/tag-firmware?offset={o}&total=40976"
                                                 for o in range(0, 40976, esl_client.TAGFW_PART)])
        self.assertEqual(parts[0][1], "application/octet-stream")
        with tempfile.NamedTemporaryFile(suffix=".bin") as f:
            f.write(b"\x02\x00" + bytes(100))   # the flasher image, not the OTA file
            f.flush()
            with redirect_stderr(io.StringIO()):
                rc, out = self.run_cli("tag-ota", f.name, "--all")
        self.assertEqual(rc, 2)

    def test_tag_ota_staged(self):
        """#306: new firmware to one tag first, then to more, then to all."""
        data = b"OSFW" + bytes([1, 2]) + (30000).to_bytes(2, "little") + bytes(8) + bytes(range(256)) * 160
        for mac, model in (("00000123456789A8", "1.54"), ("00000123456789A9", "1.54"), ("00000123456789B1", "2.9")):
            FakeAp.tags[mac] = {"mac": mac, "model": model}
        FakeAp.ap_fw = "0.10.0"
        with tempfile.NamedTemporaryFile(suffix=".bin") as f:
            f.write(data)
            f.flush()
            # a tag the AP does not know, or of another model: nothing is sent
            for to, msg in (("00000123456789FF", "the AP does not know 00000123456789FF"),
                            ("00000123456789A8,00000123456789B1", "00000123456789B1 is a 2.9 tag, not 1.54")):
                err = io.StringIO()
                with redirect_stderr(err):
                    rc, out = self.run_cli("tag-ota", f.name, "--to", to)
                self.assertEqual(rc, 2)
                self.assertIn(msg, err.getvalue())
            self.assertEqual([c for c in FakeAp.calls if "tag-firmware" in c[0]], [])
            with redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):  # --to and --all together
                    self.run_cli("tag-ota", f.name, "--to", "00000123456789A8", "--all")
                self.assertEqual(self.run_cli("tag-ota", f.name, "--model", "2.9", "--all")[0], 2)

            rc, out = self.run_cli("tag-ota", f.name, "--to", "00:00:01:23:45:67:89:a8")
            self.assertEqual(rc, 0, out)
            self.assertIn("to 00000123456789A8 ...", out)
            self.assertIn("for 1 chosen tag (00000123456789A8): 0 updated, 1 pending, 0 failed, 0 without OTA, "
                          "3 not chosen", out)
            parts = [c[0] for c in FakeAp.calls if c[0].startswith("/api/tag-firmware?")]
            self.assertEqual(parts, [f"/api/tag-firmware?offset={o}&total=40976&to=00000123456789A8"
                                     for o in range(0, 40976, esl_client.TAGFW_PART)])
            self.assertEqual(FakeAp.tagfw["to"], ["00000123456789A8"])

            # widening to all before any tag runs it is refused, unless forced
            err = io.StringIO()
            with redirect_stderr(err):
                rc, out = self.run_cli("tag-ota", "--model", "1.54", "--all")
            self.assertEqual(rc, 2)
            self.assertIn("no 1.54 tag runs 0.3.0 yet", err.getvalue())
            self.assertEqual(FakeAp.tagfw["to"], ["00000123456789A8"])

            # it has installed: the status says what is next; one more tag, then all
            FakeAp.tagfw["tags"] = {"updated": 1, "pending": 0, "failed": 0, "no_ota": 0, "not_chosen": 3}
            rc, out = self.run_cli("tag-ota")
            self.assertIn("once it has polled about 10 times without problems: tag-ota --model 1.54 --all", out)
            rc, out = self.run_cli("tag-ota", "--to", "00000123456789A9")
            self.assertEqual(rc, 0, out)
            self.assertEqual(FakeAp.tagfw["to"], ["00000123456789A8", "00000123456789A9"])
            self.assertIn("for 2 chosen tags (00000123456789A8, 00000123456789A9)", out)
            rc, out = self.run_cli("tag-ota", "--model", "1.54", "--all")
            self.assertEqual(rc, 0, out)
            self.assertEqual(FakeAp.tagfw["to"], "all")
            self.assertIn("/api/tag-firmware/1.54?to=all", [c[0] for c in FakeAp.calls])

            # several tags at once: the first with the upload, the rest one by one
            FakeAp.calls.clear()
            rc, out = self.run_cli("tag-ota", f.name, "--to", "00000123456789A8,00000123456789A9")
            self.assertEqual(rc, 0, out)
            self.assertEqual(FakeAp.tagfw["to"], ["00000123456789A8", "00000123456789A9"])
            self.assertIn("/api/tag-firmware/1.54?to=00000123456789A9", [c[0] for c in FakeAp.calls])

            # more tags for an image that is offered to all already: nothing changes, nothing is stopped
            FakeAp.tagfw["to"] = "all"
            FakeAp.calls.clear()
            rc, out = self.run_cli("tag-ota", "--to", "00000123456789A8")
            self.assertEqual(rc, 0, out)
            self.assertIn("offered to every 1.54 tag already", out)
            self.assertEqual(FakeAp.tagfw["to"], "all")
            self.assertEqual(FakeAp.calls, [])  # no POST, no DELETE

            # an AP from before "to" (0.4.4) is refused before anything is sent
            FakeAp.ap_fw = "0.4.3"
            FakeAp.calls.clear()
            err = io.StringIO()
            with redirect_stderr(err):
                rc, out = self.run_cli("tag-ota", f.name, "--to", "00000123456789A8")
            self.assertEqual(rc, 2)
            self.assertIn("the AP runs 0.4.3, which cannot offer tag firmware to chosen tags", err.getvalue())
            self.assertEqual([c for c in FakeAp.calls if "tag-firmware" in c[0]], [])
            FakeAp.ap_fw = "0.10.0"

            # an AP that still ignores "to" (no "to" in its reply, 405 on POST): the update is taken back
            FakeAp.tagfw["old_ap"] = True
            for to in ("00000123456789A8", "00000123456789A8,00000123456789A9"):
                err = io.StringIO()
                with redirect_stderr(err):
                    rc, out = self.run_cli("tag-ota", f.name, "--to", to)
                self.assertEqual(rc, 1, to)
                self.assertIn("the AP may offer it to every 1.54 tag, not the ones chosen", err.getvalue())
                self.assertIn("so the update is stopped again", err.getvalue())
                self.assertEqual(FakeAp.calls[-1], ("/api/tag-firmware/1.54", None))
                self.assertNotIn("to", FakeAp.tagfw)
            del FakeAp.tagfw["old_ap"]

            # a tag whose model the AP does not know yet: sent, with a warning
            FakeAp.tags["00000123456789B2"] = {"mac": "00000123456789B2", "model": None}
            err = io.StringIO()
            with redirect_stderr(err):
                rc, out = self.run_cli("tag-ota", f.name, "--to", "00000123456789B2")
            self.assertEqual(rc, 0, out)
            self.assertIn("does not know the model of 00000123456789B2 yet", err.getvalue())
            FakeAp.calls.clear()
            self.assertEqual(self.run_cli("tag-ota", "--delete", "1.54")[0], 0)

            # no image for the model on the AP
            err = io.StringIO()
            with redirect_stderr(err):
                rc, out = self.run_cli("tag-ota", "--to", "00000123456789A8")
            self.assertEqual(rc, 2)
            self.assertIn("the AP has no 1.54 firmware", err.getvalue())

    def test_ota(self):
        rc, out = self.run_cli("ota")
        self.assertEqual((rc, out), (0, "running 0.1.0 from ota_0, slot size 1572864 bytes, ota_1 holds nothing\n"))
        image = b"\xe9" + bytes(range(256)) * 100
        with tempfile.NamedTemporaryFile(suffix=".bin") as f:
            f.write(image)
            f.flush()
            rc, out = self.run_cli("ota", f.name)
        self.assertEqual(rc, 0)
        self.assertEqual(FakeAp.calls[-1], ("/api/ota", "application/octet-stream", image))
        self.assertIn("written to ota_1: version 0.2.0, the AP is restarting", out)
        with tempfile.NamedTemporaryFile(suffix=".bin") as f:
            f.write(b"not an image")
            f.flush()
            rc, out = self.run_cli("ota", f.name)
        self.assertEqual(rc, 2)

        # --wait: the AP answers again with the new image, or with the old one after a rollback
        cli = esl_client.Client(self.url, TOKEN)
        self.setUp()
        FakeAp.ota.update({"slot": "ota_1", "version": "0.2.0"})
        out = io.StringIO()
        with redirect_stdout(out):
            rc = esl_client.wait_back(cli, "ota_1", "0.2.0", timeout_s=5, sleep=lambda s: None)
        self.assertEqual((rc, out.getvalue()), (0, "the AP is back: version 0.2.0 from ota_1\n"))
        self.setUp()
        with redirect_stdout(out):
            rc = esl_client.wait_back(cli, "ota_1", "0.2.0", timeout_s=5, sleep=lambda s: None)
        self.assertEqual(rc, 1)
        self.assertIn("back on ota_0 with version 0.1.0", out.getvalue())
        # no answer at all: a timeout, not an exception
        dead = esl_client.Client("http://127.0.0.1:1", TOKEN, timeout=0.2, retries=1)
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = esl_client.wait_back(dead, "ota_1", "0.2.0", timeout_s=0.5, poll_s=0.1)
        self.assertEqual(rc, 1)
        self.assertIn("no answer from the AP after 0.5 s", err.getvalue())

    def test_ota_radio(self):
        radio = {"up": True, "version": "0.1.0", "fw": "1.1", "link": 2, "ota": True, "display": False}
        FakeAp.ota["radio"] = dict(radio)
        rc, out = self.run_cli("ota")
        self.assertIn("radio: 0.1.0, answering\n", out)
        image = b"\xe9" + bytes(79) + b"esl_radio" + bytes(1000)
        with tempfile.NamedTemporaryFile(suffix=".bin") as f:
            f.write(image)
            f.flush()
            rc, out = self.run_cli("ota", f.name, "--wait")
        self.assertEqual(rc, 0)
        self.assertIn("sent to the radio: version 0.2.0, the radio is restarting", out)
        self.assertIn("the radio is back: version 0.2.0", out)
        # an old radio, and one not heard from at all
        FakeAp.ota["radio"] = {"up": True, "version": None, "fw": "1.0", "link": 1, "ota": False, "display": None}
        rc, out = self.run_cli("ota")
        self.assertIn("radio: older than 0.8.2, answering; it takes new firmware only over USB", out)
        FakeAp.ota["radio"] = {"up": False, "version": None, "fw": None, "link": None, "ota": False, "display": None}
        rc, out = self.run_cli("ota")
        self.assertIn("radio: not heard from yet", out)

        # back with the old version after the grace: it went back to it
        cli = esl_client.Client(self.url, TOKEN)
        FakeAp.ota["radio"] = dict(radio)
        out = io.StringIO()
        with redirect_stdout(out):
            rc = esl_client.wait_radio_back(cli, "0.2.0", timeout_s=5, grace_s=0, sleep=lambda s: None)
        self.assertEqual(rc, 1)
        self.assertIn("went back to the old one", out.getvalue())
        FakeAp.ota["radio"] = dict(radio, up=False)
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = esl_client.wait_radio_back(cli, "0.2.0", timeout_s=0, sleep=lambda s: None)
        self.assertEqual(rc, 1)
        self.assertIn("radio's log", err.getvalue())

    def test_url_without_scheme_gets_http(self):
        self.assertEqual(esl_client.Client("192.168.0.188", TOKEN).url, "http://192.168.0.188")
        self.assertEqual(esl_client.Client("openesl.local/", TOKEN).url, "http://openesl.local")
        self.assertEqual(esl_client.Client("https://ap", TOKEN).url, "https://ap")
        self.assertEqual(esl_client.SerialClient("/dev/ttyACM0").url, "serial:/dev/ttyACM0")

    def test_ota_timeout_covers_the_upload(self):
        # urllib's timeout also bounds the whole sendall: a 1.3 MB image must get far more than 10 s
        cli = esl_client.Client("http://ap", TOKEN)
        seen = []
        cli.send = lambda *a, **kw: (seen.append((kw.get("timeout"), kw.get("retries"))),
                                     (200, b'{"version": "0.2.0"}'))[1]
        cli.ota(bytes(1300000))
        self.assertGreaterEqual(seen[0][0], 300)
        self.assertEqual(seen[0][1], 1)  # a stalled upload is not sent twice more at that timeout
        cli.ota(bytes(100))
        self.assertEqual(seen[1][0], 10.0)

    def test_same_content_is_unchanged(self):
        self.run_cli("text", "00000123456789A8", "Hello", "--frame")
        rc, out = self.run_cli("text", "00000123456789A8", "Hello", "--frame")
        self.assertIn("unchanged", out)
        self.assertEqual(FakeAp.calls[0][1], {"layout": "text", "fields": ["Hello"], "frame": True})

    def test_wait_returns_when_shown(self):
        rc, _ = self.run_cli("price", "00000123456789A8", "A", "1", "--wait", "--wait-s", "1")
        self.assertEqual(rc, 0)

    def test_date_fields(self):
        self.assertEqual(esl_client.date_fields(datetime.date(2026, 10, 17), ["Velkommen"]),
                         ["Lørdag", "17. oktober 2026", "Velkommen"])
        self.assertEqual(esl_client.date_fields(datetime.date(2026, 10, 29), ["Welcome"], "en"),
                         ["Thursday", "29 October 2026", "Welcome"])

    def test_date_lang(self):
        # the AP's, as its own date fields; else this computer's
        self.assertEqual(esl_client.date_lang({"lang": "nb"}, {"LANG": "en_US.UTF-8"}), "nb")
        self.assertEqual(esl_client.date_lang({"lang": "en"}, {"LANG": "nb_NO.UTF-8"}), "en")
        self.assertEqual(esl_client.date_lang({"tz": None}, {"LANG": "nb_NO.UTF-8"}), "nb")
        self.assertEqual(esl_client.date_lang(None, {"LANG": "en_GB.UTF-8", "LC_TIME": "nn_NO"}), "nb")
        self.assertEqual(esl_client.date_lang({}, {"LANG": "no"}), "nb")
        self.assertEqual(esl_client.date_lang({}, {"LANG": "C.UTF-8"}), "en")
        self.assertEqual(esl_client.date_lang({}, {}), "en")
        self.assertEqual(esl_client.date_lang({"lang": None}, {"LANG": "nb_NO.UTF-8"}), "nb")   # not chosen
        self.assertEqual(esl_client.date_lang({}, {"LANG": "Norwegian_Norway.1252"}), "nb")   # as Windows names it

    def test_date_in_the_aps_language(self):
        FakeAp.config["lang"] = "en"
        rc, out = self.run_cli("date", "0000000000000001")
        self.assertEqual(rc, 0, out)
        self.assertEqual(FakeAp.calls[-1][1]["fields"], esl_client.date_fields(datetime.date.today(), (), "en"))

    def test_date_today(self):
        FakeAp.config["lang"] = "nb"
        rc, out = self.run_cli("date", "0000000000000001", "Velkommen", "--frame")
        self.assertEqual(rc, 0, out)
        path, body = FakeAp.calls[0]
        self.assertEqual(path, "/api/tags/0000000000000001")
        self.assertEqual(body, {"layout": "text", "frame": True,
                                "fields": esl_client.date_fields(datetime.date.today(), ["Velkommen"])})

    def test_date_days_are_scheduled_at_midnight(self):
        FakeAp.config["lang"] = "nb"
        rc, out = self.run_cli("date", "0000000000000001", "--days", "3")
        self.assertEqual(rc, 0, out)
        self.assertEqual(FakeAp.calls[0][0], "/api/time")   # the AP's clock is checked first
        rows = FakeAp.calls[-1][1]["tags"]
        today = datetime.date.today()
        days = [today + datetime.timedelta(days=i) for i in range(3)]
        self.assertEqual([r["fields"] for r in rows], [esl_client.date_fields(d) for d in days])
        self.assertNotIn("at", rows[0])
        self.assertEqual([r["at"] for r in rows[1:]],
                         [int(datetime.datetime.combine(d, datetime.time()).timestamp()) for d in days[1:]])

    def test_date_too_many_lines(self):
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.run_cli("date", "0000000000000001", "a", "b", "c")

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

    def test_set_many_16_fields(self):
        # 50 tags with 16 fields are about 1,150 JSON tokens: more than the AP takes in one body
        rows = [{"mac": f"{i:016X}", "layout": "text", "fields": [f"f{k}" for k in range(16)]} for i in range(120)]
        self.assertGreater(esl_client.json_tokens({"tags": rows[:50]}), 1024)
        cli = esl_client.Client(self.url, TOKEN)
        self.assertEqual(len(cli.set_many(rows)), 120)
        bodies = [b for p, b in FakeAp.calls if p == "/api/tags"]
        self.assertEqual(sum(len(b["tags"]) for b in bodies), 120)
        for b in bodies:
            self.assertLessEqual(esl_client.json_tokens(b), 1024)
            self.assertLessEqual(len(json.dumps(b).encode()), esl_client.BODY_MAX_BYTES)

    def test_batches(self):
        small = [{"mac": f"{i:016X}", "fields": ["a"]} for i in range(120)]
        self.assertEqual([len(c) for c in esl_client.batches(small)], [50, 50, 20])
        big = [{"mac": f"{i:016X}", "fields": ["x" * 255] * 16} for i in range(10)]
        parts = list(esl_client.batches(big))
        self.assertEqual(sum(len(c) for c in parts), 10)
        self.assertTrue(all(len(json.dumps({"tags": c})) <= esl_client.BODY_MAX_BYTES for c in parts))
        self.assertEqual(list(esl_client.batches([])), [])

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
        self.assertIn("no API token set", out)
        rc, out = self.run_cli("wifi", "--new-token", "lang-hemmelig")
        self.assertEqual(rc, 0)
        self.assertEqual(FakeAp.calls[-1], ("/api/wifi", {"token": "lang-hemmelig"}))
        self.assertIn("trying to join 'Butikken'", out)
        self.assertNotIn("no API token set", out)
        self.assertIn("API token changed", out)
        rc, out = self.run_cli("wifi", "Home", "--password", "hemmelig123", "--new-token", "lang-hemmelig")
        self.assertEqual(FakeAp.calls[-1], ("/api/wifi", {"ssid": "Home", "password": "hemmelig123", "token": "lang-hemmelig"}))
        rc, out = self.run_cli("wifi", "--hostname", "butikk")
        self.assertEqual(FakeAp.calls[-1], ("/api/wifi", {"hostname": "butikk"}))
        self.assertIn("name 'butikk' (butikk.local)", out)

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
        self.addCleanup(lambda: cli.fd is not None and os.close(cli.fd))
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
        self.assertIsNone(cli.fd)   # opened again on the next request, in case the AP restarted


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
