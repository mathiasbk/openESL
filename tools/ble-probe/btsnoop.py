#!/usr/bin/env python3
"""btsnoop.py - what a phone app wrote to a Bluetooth tag, from the phone's HCI log.

Reads a btsnoop file (Android's "Bluetooth HCI snoop log", btsnoop_hci.log, or a capture
saved from Wireshark in that format) and prints the GATT traffic per connection: every
write the app made, every notification and indication the tag sent, and reads, with the
characteristic's UUID when the app discovered the services in the same log.

  python3 tools/ble-probe/btsnoop.py btsnoop_hci.log
  python3 tools/ble-probe/btsnoop.py btsnoop_hci.log --addr AA:BB:CC:DD:EE:FF
  python3 tools/ble-probe/btsnoop.py btsnoop_hci.log --summary
  python3 tools/ble-probe/btsnoop.py btsnoop_hci.log --json > writes.json

This is how a tag's protocol is learnt for openESL: record the maker's app sending a
known picture, then read here which characteristic gets what, in what order and in what
pieces. See README.md in this folder.

Standard library only.
"""
import argparse
import json
import struct
import sys

MAGIC = b"btsnoop\x00"
DATALINK_H1 = 1001  # HCI packets without a type byte; the type is in the record's flags
DATALINK_H4 = 1002  # HCI UART: one type byte before each packet
# Microseconds from year 0 to 1970-01-01, as btsnoop counts its timestamps
EPOCH_DELTA_US = 0x00DCDDB30F2F8000

H4_CMD, H4_ACL, H4_SCO, H4_EVT = 1, 2, 3, 4

CID_ATT = 0x0004

ATT_OPS = {
    0x01: "ERROR",
    0x02: "MTU_REQ",
    0x03: "MTU_RSP",
    0x04: "FIND_INFO_REQ",
    0x05: "FIND_INFO_RSP",
    0x06: "FIND_BY_TYPE_REQ",
    0x07: "FIND_BY_TYPE_RSP",
    0x08: "READ_BY_TYPE_REQ",
    0x09: "READ_BY_TYPE_RSP",
    0x0A: "READ_REQ",
    0x0B: "READ_RSP",
    0x0C: "READ_BLOB_REQ",
    0x0D: "READ_BLOB_RSP",
    0x10: "READ_BY_GROUP_REQ",
    0x11: "READ_BY_GROUP_RSP",
    0x12: "WRITE_REQ",
    0x13: "WRITE_RSP",
    0x16: "PREPARE_WRITE_REQ",
    0x17: "PREPARE_WRITE_RSP",
    0x18: "EXECUTE_WRITE_REQ",
    0x19: "EXECUTE_WRITE_RSP",
    0x1B: "NOTIFY",
    0x1D: "INDICATE",
    0x1E: "CONFIRM",
    0x52: "WRITE_CMD",
}
# The operations that carry a characteristic's value, and are printed by default
VALUE_OPS = {"WRITE_REQ", "WRITE_CMD", "NOTIFY", "INDICATE", "READ_REQ", "READ_RSP",
             "PREPARE_WRITE_REQ", "EXECUTE_WRITE_REQ", "ERROR", "MTU_REQ", "MTU_RSP"}

UUID_CHAR_DECL = 0x2803
UUID_PRIMARY = 0x2800
UUID_CCCD = 0x2902


class Record:
    __slots__ = ("time_us", "sent", "kind", "data")

    def __init__(self, time_us, sent, kind, data):
        self.time_us = time_us  # since 1970, in microseconds
        self.sent = sent  # True: from the phone to its controller (towards the tag)
        self.kind = kind  # H4_CMD, H4_ACL, H4_SCO or H4_EVT
        self.data = data  # the HCI packet without a type byte


def read_records(f):
    """The HCI packets in a btsnoop file, in order."""
    head = f.read(16)
    if len(head) < 16 or head[:8] != MAGIC:
        raise ValueError("not a btsnoop file (no 'btsnoop' magic at the start)")
    version, datalink = struct.unpack(">II", head[8:])
    if version != 1:
        raise ValueError("btsnoop version %d, only version 1 is known" % version)
    if datalink not in (DATALINK_H1, DATALINK_H4):
        raise ValueError("btsnoop data link type %d, only 1001 (HCI) and 1002 (HCI UART) are known" % datalink)
    while True:
        rh = f.read(24)
        if not rh:
            return
        if len(rh) < 24:
            return  # a log cut off in the middle of a record: keep what came before
        _orig, incl, flags, _drops, ts = struct.unpack(">IIIIq", rh)
        data = f.read(incl)
        if len(data) < incl:
            return
        sent = not (flags & 1)
        if datalink == DATALINK_H4:
            if not data:
                continue
            kind, data = data[0], data[1:]
        else:
            kind = (H4_EVT if not sent else H4_CMD) if flags & 2 else H4_ACL
        yield Record(ts - EPOCH_DELTA_US, sent, kind, data)


def fmt_addr(b):
    """A Bluetooth address as HCI carries it (6 bytes, least significant first)."""
    return ":".join("%02X" % x for x in reversed(b))


def fmt_uuid(b):
    if len(b) == 2:
        return "%04x" % struct.unpack("<H", b)[0]
    if len(b) == 16:
        h = b[::-1].hex()
        return "%s-%s-%s-%s-%s" % (h[:8], h[8:12], h[12:16], h[16:20], h[20:])
    return b.hex()


class Conn:
    """One LE connection: its peer, the characteristics it found and the ATT PDUs."""

    def __init__(self, handle, addr=None, t_us=0):
        self.handle = handle
        self.addr = addr
        self.t0_us = t_us
        self.chars = {}  # value handle -> {"uuid", "props"}
        self.cccds = set()  # handles of Client Characteristic Configuration descriptors
        self.pending_type = None  # the type the last READ_BY_TYPE_REQ / FIND_INFO asked for
        self.frag = {True: None, False: None}  # L2CAP being put together, per direction
        self.mtu = None


class Reader:
    """Turns HCI records into ATT events per connection."""

    def __init__(self):
        self.conns = {}  # current connection handle -> Conn
        self.done = []  # connections that ended, in order
        self.events = []

    def conn(self, handle, t_us):
        c = self.conns.get(handle)
        if c is None:  # traffic on a connection made before the log started
            c = self.conns[handle] = Conn(handle, None, t_us)
        return c

    def feed(self, rec):
        if rec.kind == H4_EVT:
            self.on_event(rec)
        elif rec.kind == H4_ACL:
            self.on_acl(rec)

    def on_event(self, rec):
        d = rec.data
        if len(d) < 2:
            return
        code, plen = d[0], d[1]
        p = d[2:2 + plen]
        if code == 0x3E and p:  # LE meta event
            sub = p[0]
            # LE Connection Complete (0x01) and LE Enhanced Connection Complete (0x0A, 0x29):
            # status, handle, role, peer address type, peer address
            if sub in (0x01, 0x0A, 0x29) and len(p) >= 11 and p[1] == 0:
                handle = struct.unpack("<H", p[2:4])[0] & 0x0FFF
                old = self.conns.pop(handle, None)
                if old:
                    self.done.append(old)
                self.conns[handle] = Conn(handle, fmt_addr(p[6:12]), rec.time_us)
                self.events.append({"t_us": rec.time_us, "conn": self.conns[handle], "op": "CONNECT", "data": b""})
        elif code == 0x05 and len(p) >= 4 and p[0] == 0:  # Disconnection Complete
            handle = struct.unpack("<H", p[1:3])[0] & 0x0FFF
            c = self.conns.pop(handle, None)
            if c:
                self.events.append({"t_us": rec.time_us, "conn": c, "op": "DISCONNECT",
                                    "data": bytes([p[3]])})
                self.done.append(c)

    def on_acl(self, rec):
        d = rec.data
        if len(d) < 4:
            return
        hf, dlen = struct.unpack("<HH", d[:4])
        handle, pb = hf & 0x0FFF, (hf >> 12) & 3
        payload = d[4:4 + dlen]
        c = self.conn(handle, rec.time_us)
        if pb == 1:  # continuation of an L2CAP frame
            buf = c.frag[rec.sent]
            if buf is None:
                return
            buf += payload
        else:
            buf = bytearray(payload)
        if len(buf) < 4:
            c.frag[rec.sent] = buf
            return
        l2len, cid = struct.unpack("<HH", buf[:4])
        if len(buf) < 4 + l2len:
            c.frag[rec.sent] = buf
            return
        c.frag[rec.sent] = None
        if cid == CID_ATT:
            self.on_att(c, rec, bytes(buf[4:4 + l2len]))

    def on_att(self, c, rec, pdu):
        if not pdu:
            return
        op = ATT_OPS.get(pdu[0], "OP_%02X" % pdu[0])
        body = pdu[1:]
        ev = {"t_us": rec.time_us, "conn": c, "op": op, "sent": rec.sent, "handle": None, "data": body}
        if op in ("WRITE_REQ", "WRITE_CMD", "NOTIFY", "INDICATE", "READ_REQ", "READ_BLOB_REQ") and len(body) >= 2:
            ev["handle"] = struct.unpack("<H", body[:2])[0]
            ev["data"] = body[2:]
            if op == "READ_BLOB_REQ":
                ev["data"] = b""
            if op == "WRITE_REQ" and ev["handle"] in c.cccds:
                ev["op"] = "SUBSCRIBE"
        elif op == "PREPARE_WRITE_REQ" and len(body) >= 4:
            ev["handle"] = struct.unpack("<H", body[:2])[0]
            ev["data"] = body[4:]
        elif op in ("MTU_REQ", "MTU_RSP") and len(body) >= 2:
            c.mtu = struct.unpack("<H", body[:2])[0]
        elif op == "READ_BY_TYPE_REQ" and len(body) >= 6:
            c.pending_type = body[4:]
        elif op == "READ_BY_TYPE_RSP" and body:
            self.learn_by_type(c, body)
        elif op == "FIND_INFO_RSP" and body:
            self.learn_info(c, body)
        self.events.append(ev)

    def learn_by_type(self, c, body):
        """Characteristic declarations: the value handle, properties and UUID of each."""
        if c.pending_type != struct.pack("<H", UUID_CHAR_DECL):
            return
        n = body[0]
        if n < 2:
            return
        for i in range(1, len(body) - n + 1, n):
            ent = body[i:i + n]
            val = ent[2:]
            if len(val) >= 5:
                props = val[0]
                vh = struct.unpack("<H", val[1:3])[0]
                c.chars[vh] = {"uuid": fmt_uuid(val[3:]), "props": props}

    def learn_info(self, c, body):
        """Descriptors: notes the CCCDs, so writing one shows as SUBSCRIBE."""
        fmt = body[0]
        size = 4 if fmt == 1 else 18
        for i in range(1, len(body) - size + 1, size):
            h = struct.unpack("<H", body[i:i + 2])[0]
            if body[i + 2:i + size] == struct.pack("<H", UUID_CCCD):
                c.cccds.add(h)

    def all_conns(self):
        return self.done + list(self.conns.values())


PROPS = ((0x02, "read"), (0x04, "write-no-rsp"), (0x08, "write"), (0x10, "notify"), (0x20, "indicate"))


def fmt_props(p):
    return ",".join(name for bit, name in PROPS if p & bit)


def read_file(path):
    r = Reader()
    with open(path, "rb") as f:
        for rec in read_records(f):
            r.feed(rec)
    return r


def wanted(ev, args):
    c = ev["conn"]
    if args.addr and (c.addr or "").upper() != args.addr.upper():
        return False
    if args.handle is not None and ev.get("handle") != args.handle:
        return False
    if args.all:
        return True
    return ev["op"] in VALUE_OPS or ev["op"] in ("CONNECT", "DISCONNECT", "SUBSCRIBE")


def uuid_of(c, h):
    ch = c.chars.get(h)
    return ch["uuid"] if ch else None


def print_events(r, args, out):
    t0 = None
    for ev in r.events:
        if not wanted(ev, args):
            continue
        if t0 is None:
            t0 = ev["t_us"]
        c = ev["conn"]
        who = c.addr or ("handle 0x%03x" % c.handle)
        arrow = "" if ev["op"] in ("CONNECT", "DISCONNECT") else ("->" if ev.get("sent") else "<-")
        line = "%9.3f %s %2s %-17s" % ((ev["t_us"] - t0) / 1e6, who, arrow, ev["op"])
        if ev.get("handle") is not None:
            u = uuid_of(c, ev["handle"])
            line += " h=0x%04x" % ev["handle"] + (" (%s)" % u if u else "")
        data = ev["data"]
        if ev["op"] == "DISCONNECT":
            line += " reason 0x%02x" % data[0]
        elif data:
            shown = data if args.full or len(data) <= 32 else data[:32]
            line += " %3d B %s%s" % (len(data), shown.hex(" "), "" if shown is data else " ...")
        out.write(line + "\n")


def summary(r, args, out):
    for c in r.all_conns():
        if args.addr and (c.addr or "").upper() != args.addr.upper():
            continue
        out.write("Connection to %s%s\n" % (c.addr or "an unknown peer (connected before the log)",
                                            ", MTU %d" % c.mtu if c.mtu else ""))
        if c.chars:
            out.write("  characteristics:\n")
            for h in sorted(c.chars):
                ch = c.chars[h]
                out.write("    0x%04x %s [%s]\n" % (h, ch["uuid"], fmt_props(ch["props"])))
        per = {}
        for ev in r.events:
            if ev["conn"] is not c or ev.get("handle") is None:
                continue
            k = (ev["handle"], ev["op"])
            n, size = per.get(k, (0, 0))
            per[k] = (n + 1, size + len(ev["data"]))
        if per:
            out.write("  traffic:\n")
            for (h, op), (n, size) in sorted(per.items()):
                u = uuid_of(c, h)
                out.write("    0x%04x %-17s %5d x %7d B%s\n" % (h, op, n, size, "  " + u if u else ""))


def to_json(r, args):
    rows = []
    for ev in r.events:
        if not wanted(ev, args):
            continue
        c = ev["conn"]
        rows.append({
            "t": ev["t_us"] / 1e6,
            "addr": c.addr,
            "dir": None if ev["op"] in ("CONNECT", "DISCONNECT") else ("to_tag" if ev.get("sent") else "from_tag"),
            "op": ev["op"],
            "handle": ev.get("handle"),
            "uuid": uuid_of(c, ev["handle"]) if ev.get("handle") is not None else None,
            "data": ev["data"].hex(),
        })
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="btsnoop file (btsnoop_hci.log)")
    ap.add_argument("--addr", help="only the connection to this tag (AA:BB:CC:DD:EE:FF)")
    ap.add_argument("--handle", type=lambda s: int(s, 0), help="only this attribute handle (0x21 or 33)")
    ap.add_argument("--all", action="store_true", help="also service discovery and responses")
    ap.add_argument("--full", action="store_true", help="whole values, not the first 32 bytes")
    ap.add_argument("--summary", action="store_true", help="per connection: characteristics and how much went where")
    ap.add_argument("--json", action="store_true", help="the events as JSON")
    args = ap.parse_args(argv)
    try:
        r = read_file(args.log)
    except (OSError, ValueError) as e:
        sys.stderr.write("btsnoop.py: %s\n" % e)
        return 1
    if args.json:
        json.dump(to_json(r, args), sys.stdout, indent=1)
        sys.stdout.write("\n")
    elif args.summary:
        summary(r, args, sys.stdout)
    else:
        print_events(r, args, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
