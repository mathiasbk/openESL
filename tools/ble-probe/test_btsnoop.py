"""Tests for btsnoop.py, on logs built here byte by byte (python3 -m unittest test_btsnoop)."""
import io
import json
import os
import struct
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from types import SimpleNamespace

import btsnoop as bs

PEER = bytes([0x66, 0x55, 0x44, 0x33, 0x22, 0x11])  # 11:22:33:44:55:66 on the air
HANDLE = 0x0040
T0 = bs.EPOCH_DELTA_US + 1_700_000_000_000_000


def event(code, params):
    return bytes([code, len(params)]) + params


def conn_complete(handle=HANDLE):
    # LE meta, Connection Complete: status, handle, role, peer address type, address,
    # interval, latency, timeout, clock accuracy
    p = bytes([0x01, 0x00]) + struct.pack("<H", handle) + bytes([0x00, 0x00]) + PEER + bytes(7)
    return event(0x3E, p)


def disconn(handle=HANDLE, reason=0x13):
    return event(0x05, bytes([0x00]) + struct.pack("<H", handle) + bytes([reason]))


def acl(att, handle=HANDLE, split=None):
    """ACL packets carrying one ATT PDU; split cuts the L2CAP frame into two packets."""
    l2 = struct.pack("<HH", len(att), bs.CID_ATT) + att
    if split is None:
        return [struct.pack("<HH", handle | 0x2000, len(l2)) + l2]
    a, b = l2[:split], l2[split:]
    return [struct.pack("<HH", handle | 0x2000, len(a)) + a,
            struct.pack("<HH", handle | 0x1000, len(b)) + b]


def build(packets, datalink=bs.DATALINK_H4):
    """packets: (sent, kind, data). A btsnoop file as bytes."""
    out = bytearray(bs.MAGIC + struct.pack(">II", 1, datalink))
    for i, (sent, kind, data) in enumerate(packets):
        if datalink == bs.DATALINK_H4:
            body = bytes([kind]) + data
            flags = 0 if sent else 1
        else:
            body = data
            flags = (0 if sent else 1) | (2 if kind in (bs.H4_CMD, bs.H4_EVT) else 0)
        out += struct.pack(">IIIIq", len(body), len(body), flags, 0, T0 + i * 1000) + body
    return bytes(out)


def session(datalink=bs.DATALINK_H4):
    pk = [(False, bs.H4_EVT, conn_complete())]
    def att(sent, pdu, split=None):
        for a in acl(pdu, split=split):
            pk.append((sent, bs.H4_ACL, a))
    att(True, bytes([0x02]) + struct.pack("<H", 247))  # MTU_REQ
    att(False, bytes([0x03]) + struct.pack("<H", 185))  # MTU_RSP
    # READ_BY_TYPE_REQ for characteristic declarations 0x0001-0xffff
    att(True, bytes([0x08]) + struct.pack("<HHH", 1, 0xFFFF, bs.UUID_CHAR_DECL))
    # two declarations: 0x0020 -> value 0x0021 (write-no-rsp, notify, 16-bit fef1),
    # and 0x0023 -> value 0x0024 (write, 128-bit)
    u128 = bytes(range(16))
    rsp = bytes([0x09, 7]) + struct.pack("<HBH", 0x0020, 0x14, 0x0021) + struct.pack("<H", 0xFEF1)
    att(False, rsp)
    att(False, bytes([0x09, 21]) + struct.pack("<HBH", 0x0023, 0x08, 0x0024) + u128)
    # FIND_INFO_RSP: 0x0022 is the CCCD of 0x0021
    att(False, bytes([0x05, 0x01]) + struct.pack("<HH", 0x0022, bs.UUID_CCCD))
    att(True, bytes([0x12]) + struct.pack("<H", 0x0022) + b"\x01\x00")  # subscribe
    att(True, bytes([0x52]) + struct.pack("<H", 0x0021) + bytes(range(40)), split=10)  # in two ACL packets
    att(False, bytes([0x1B]) + struct.pack("<H", 0x0021) + b"\xaa\x01")  # notify
    att(True, bytes([0x12]) + struct.pack("<H", 0x0024) + b"\x05")  # write request
    pk.append((False, bs.H4_EVT, disconn()))
    return build(pk, datalink)


def reader_of(blob):
    r = bs.Reader()
    for rec in bs.read_records(io.BytesIO(blob)):
        r.feed(rec)
    return r


class TestBtsnoop(unittest.TestCase):
    def check_session(self, r):
        conns = r.all_conns()
        self.assertEqual(len(conns), 1)
        c = conns[0]
        self.assertEqual(c.addr, "11:22:33:44:55:66")
        self.assertEqual(c.mtu, 185)
        self.assertEqual(c.chars[0x0021], {"uuid": "fef1", "props": 0x14})
        self.assertEqual(c.chars[0x0024]["uuid"], "0f0e0d0c-0b0a-0908-0706-050403020100")
        self.assertIn(0x0022, c.cccds)
        ops = [(e["op"], e.get("handle")) for e in r.events if e["op"] in
               ("CONNECT", "SUBSCRIBE", "WRITE_CMD", "NOTIFY", "WRITE_REQ", "DISCONNECT")]
        self.assertEqual(ops, [("CONNECT", None), ("SUBSCRIBE", 0x22), ("WRITE_CMD", 0x21),
                               ("NOTIFY", 0x21), ("WRITE_REQ", 0x24), ("DISCONNECT", None)])
        w = [e for e in r.events if e["op"] == "WRITE_CMD"][0]
        self.assertEqual(w["data"], bytes(range(40)))  # put back together from two packets
        self.assertTrue(w["sent"])
        n = [e for e in r.events if e["op"] == "NOTIFY"][0]
        self.assertFalse(n["sent"])

    def test_h4(self):
        self.check_session(reader_of(session(bs.DATALINK_H4)))

    def test_h1(self):
        self.check_session(reader_of(session(bs.DATALINK_H1)))

    def test_not_btsnoop(self):
        with self.assertRaises(ValueError):
            list(bs.read_records(io.BytesIO(b"pcap file here...")))

    def test_cut_off_log(self):
        blob = session()
        r = reader_of(blob[:-5])  # the last record is cut: the rest is still read
        self.assertTrue(any(e["op"] == "WRITE_REQ" for e in r.events))
        self.assertFalse(any(e["op"] == "DISCONNECT" for e in r.events))

    def test_traffic_before_connect(self):
        # A log started while connected: no address, but the writes are still there
        pk = [(True, bs.H4_ACL, a) for a in acl(bytes([0x52]) + struct.pack("<H", 0x30) + b"\x01\x02")]
        r = reader_of(build(pk))
        c = r.all_conns()[0]
        self.assertIsNone(c.addr)
        self.assertEqual([e["op"] for e in r.events], ["WRITE_CMD"])

    def test_filtered_log_warns(self):
        # Android's "Enabled Filtered" keeps only the start of each data packet: the write is
        # gone, and main says why on stderr
        blob = bytearray(bs.MAGIC + struct.pack(">II", 1, bs.DATALINK_H4))
        pk = [(False, bs.H4_EVT, conn_complete())]
        pk += [(True, bs.H4_ACL, a) for a in acl(bytes([0x52]) + struct.pack("<H", 0x21) + bytes(100))]
        for i, (sent, kind, data) in enumerate(pk):
            body = bytes([kind]) + data
            incl = body if kind == bs.H4_EVT else body[:20]
            blob += struct.pack(">IIIIq", len(body), len(incl), 0 if sent else 1, 0, T0 + i) + incl
        r = reader_of(bytes(blob))
        self.assertEqual(r.cut, 1)
        self.assertFalse(any(e["op"] == "WRITE_CMD" for e in r.events))
        with tempfile.NamedTemporaryFile(suffix=".log", delete=False) as f:
            f.write(bytes(blob))
        try:
            err = io.StringIO()
            with redirect_stdout(io.StringIO()), redirect_stderr(err):
                self.assertEqual(bs.main([f.name]), 0)
            self.assertIn("Enabled Filtered", err.getvalue())
        finally:
            os.unlink(f.name)

    def test_classic_link_hidden(self):
        # A classic ACL link (headphones) that carries no ATT: neither its disconnect nor an
        # empty block in the summary
        other = 0x0003
        pk = [(True, bs.H4_ACL, struct.pack("<HH", other | 0x2000, 8) + struct.pack("<HH", 4, 0x0041) + b"abcd"),
              (False, bs.H4_EVT, disconn(other))]
        r = reader_of(build(pk))
        self.assertEqual(r.events, [])
        out = io.StringIO()
        bs.summary(r, SimpleNamespace(addr=None), out)
        self.assertEqual(out.getvalue(), "")

    def test_discovery_only_from_tag_answers(self):
        # The tag asks the phone for its characteristics: the phone's answer is not the tag's table
        pk = [(False, bs.H4_EVT, conn_complete())]
        for sent, pdu in ((False, bytes([0x08]) + struct.pack("<HHH", 1, 0xFFFF, bs.UUID_CHAR_DECL)),
                          (True, bytes([0x09, 7]) + struct.pack("<HBH", 0x0010, 0x02, 0x0011) + struct.pack("<H", 0x2A00))):
            pk += [(sent, bs.H4_ACL, a) for a in acl(pdu)]
        r = reader_of(build(pk))
        self.assertEqual(r.all_conns()[0].chars, {})

    def test_read_response_has_handle(self):
        pk = [(False, bs.H4_EVT, conn_complete())]
        for sent, pdu in ((True, bytes([0x0A]) + struct.pack("<H", 0x0030)), (False, bytes([0x0B]) + b"\x64")):
            pk += [(sent, bs.H4_ACL, a) for a in acl(pdu)]
        r = reader_of(build(pk))
        rsp = [e for e in r.events if e["op"] == "READ_RSP"][0]
        self.assertEqual(rsp["handle"], 0x30)
        self.assertEqual(rsp["data"], b"\x64")

    def test_btmon_refused(self):
        with self.assertRaises(ValueError) as cm:
            list(bs.read_records(io.BytesIO(bs.MAGIC + struct.pack(">II", 1, 2001))))
        self.assertIn("Wireshark", str(cm.exception))

    def run_main(self, *args):
        with tempfile.NamedTemporaryFile(suffix=".log", delete=False) as f:
            f.write(session())
        try:
            out = io.StringIO()
            with redirect_stdout(out):
                code = bs.main([f.name] + list(args))
            return code, out.getvalue()
        finally:
            os.unlink(f.name)

    def test_main_text(self):
        code, out = self.run_main()
        self.assertEqual(code, 0)
        self.assertIn("WRITE_CMD", out)
        self.assertIn("h=0x0021 (fef1)", out)
        self.assertIn("40 B 00 01 02", out)
        self.assertNotIn("READ_BY_TYPE_REQ", out)  # discovery only with --all
        _, out = self.run_main("--all")
        self.assertIn("READ_BY_TYPE_REQ", out)

    def test_main_filters(self):
        _, out = self.run_main("--handle", "0x24")
        self.assertIn("WRITE_REQ", out)
        self.assertNotIn("WRITE_CMD", out)
        _, out = self.run_main("--addr", "00:00:00:00:00:01")
        self.assertEqual(out, "")

    def test_main_summary_and_json(self):
        _, out = self.run_main("--summary")
        self.assertIn("Connection to 11:22:33:44:55:66, MTU 185", out)
        self.assertIn("0x0021 fef1 [write-no-rsp,notify]", out)
        self.assertIn("WRITE_CMD", out)
        _, out = self.run_main("--json")
        rows = json.loads(out)
        w = [r for r in rows if r["op"] == "WRITE_CMD"][0]
        self.assertEqual(w["dir"], "to_tag")
        self.assertEqual(w["uuid"], "fef1")
        self.assertEqual(w["data"], bytes(range(40)).hex())

    def test_long_value_cut_unless_full(self):
        _, out = self.run_main("--handle", "0x21")
        self.assertIn("...", out)
        _, out = self.run_main("--handle", "0x21", "--full")
        self.assertIn(bytes(range(40)).hex(" "), out)


if __name__ == "__main__":
    unittest.main()
