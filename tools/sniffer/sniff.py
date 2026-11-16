#!/usr/bin/env python3
"""
Reads the AP sniffer (python ap/build.py --sniffer <board>, docs/radio-sniffer.md) over its USB
port and prints every 802.15.4 frame it hears, decoded: the MAC header, the openESL message in
it, and the time since the frame before, which is what tells whether an answer came inside a
tag's listening window.

  sniff.py /dev/ttyACM0                     every frame on the sniffer's channel (25 when built)
  sniff.py /dev/ttyACM0 --channel 20        tune it to channel 20 first (not kept over a restart)
  sniff.py /dev/ttyACM0 --hop 300           hop through 11-26, 300 ms per channel
  sniff.py /dev/ttyACM0 --pan 0x4553        only frames to or from that PAN
  sniff.py /dev/ttyACM0 --mac 0000021234567890   only frames to or from that tag
  sniff.py /dev/ttyACM0 --pcap out.pcap     also write a capture for Wireshark
  sniff.py /dev/ttyACM0 --log out.log       also keep the sniffer's raw lines
  sniff.py --read out.log                   decode raw lines saved before (or copied from a terminal)

The port is opened with DTR and RTS held low. A C6 restarts when a terminal raises them, and a
sniffer put on with OTA never confirms itself, so that restart would take it back to the AP
firmware. Needs pyserial on Windows; on Linux and macOS it uses termios only.
"""
import argparse
import os
import struct
import sys
import time

# shared/proto.h
MSG_NAMES = {0x01: "POLL", 0x02: "IDLE", 0x06: "FW", 0x07: "FW_REQ", 0x08: "CONFIG", 0x09: "IMG", 0x0A: "IMG_REQ"}
POLL_FLAGS = [(0x01, "booted"), (0x02, "drawfail"), (0x08, "ota"), (0x10, "fwfail"), (0x20, "image"),
              (0x40, "rle"), (0x80, "2d")]
LINK_FLAGS = [(0x01, "retry"), (0x02, "lost_long"), (0x04, "any_ap")]
FRAME_TYPES = {0: "beacon", 1: "data", 2: "ack", 3: "cmd"}
AP_SHORT = 0x0000
FCF_TAG_TO_AP = 0xC841
FCF_AP_TO_TAG = 0x8C41
LINKTYPE_IEEE802_15_4_NOFCS = 230


def parse_line(line):
    """One "@SNF <rx_us> <channel> <rssi> <lqi> <hex>" line (ap/main/radio.c, sniff_line) as a dict,
    or None for any other line (the AP's log, @SNF-ST, ...). The line may have text before @SNF."""
    at = line.find("@SNF ")
    if at < 0:
        return None
    parts = line[at:].split()
    if len(parts) not in (5, 6):
        return None
    try:
        frame = bytes.fromhex(parts[5]) if len(parts) == 6 else b""
        return {"us": int(parts[1]), "channel": int(parts[2]), "rssi": int(parts[3]), "lqi": int(parts[4]),
                "frame": frame}
    except ValueError:
        return None


def mac_str(b):
    """An extended address as openESL shows it (the dashboard, GET /api/tags): the 8 bytes
    on the air are little-endian"""
    return b[::-1].hex().upper()


def decode_mac(frame):
    """The 802.15.4 MAC header: a dict with type, seq, the addresses and the payload offset,
    or None when the frame is too short for the header its FCF announces"""
    if len(frame) < 3:
        return None
    fcf = struct.unpack_from("<H", frame)[0]
    h = {"fcf": fcf, "type": fcf & 7, "security": bool(fcf & 0x08), "pending": bool(fcf & 0x10),
         "ack_req": bool(fcf & 0x20), "pan_comp": bool(fcf & 0x40), "dst_mode": (fcf >> 10) & 3,
         "version": (fcf >> 12) & 3, "src_mode": (fcf >> 14) & 3, "seq": frame[2],
         "dst_pan": None, "dst": None, "src_pan": None, "src": None}
    off = 3

    def take(n):
        nonlocal off
        if off + n > len(frame):
            raise IndexError
        v = frame[off:off + n]
        off += n
        return v

    try:
        if h["dst_mode"]:
            h["dst_pan"] = struct.unpack("<H", take(2))[0]
            a = take(2 if h["dst_mode"] == 2 else 8)
            h["dst"] = "%04X" % struct.unpack("<H", a)[0] if len(a) == 2 else mac_str(a)
        if h["src_mode"]:
            if not h["pan_comp"] or not h["dst_mode"]:
                h["src_pan"] = struct.unpack("<H", take(2))[0]
            else:
                h["src_pan"] = h["dst_pan"]
            a = take(2 if h["src_mode"] == 2 else 8)
            h["src"] = "%04X" % struct.unpack("<H", a)[0] if len(a) == 2 else mac_str(a)
    except IndexError:
        return None
    h["payload"] = off
    return h


def decode_esl(h, payload):
    """The openESL message in a frame with openESL's addressing (docs/protocol.md), as text, or ""."""
    if h["fcf"] not in (FCF_TAG_TO_AP, FCF_AP_TO_TAG) or len(payload) < 2:
        return ""
    name = MSG_NAMES.get(payload[0], "type 0x%02X" % payload[0])
    text = name if payload[1] == 1 else "%s proto_ver %d" % (name, payload[1])
    if payload[0] == 0x01 and len(payload) >= 12:
        fw, mv, cid, flags, model = struct.unpack_from("<HHIBB", payload, 2)
        names = [n for bit, n in POLL_FLAGS if flags & bit]
        text += " fw=%d bat=%dmV id=%08X model=%d" % (fw, mv, cid, model)
        if names:
            text += " [%s]" % ",".join(names)
        if len(payload) >= 18:
            text += " lost=%d" % payload[16]
            lf = [n for bit, n in LINK_FLAGS if payload[17] & bit]
            if lf:
                text += " link[%s]" % ",".join(lf)
        if len(payload) >= 22:
            text += " apid=%08X" % struct.unpack_from("<I", payload, 18)[0]
    elif payload[0] in (0x02, 0x06, 0x08, 0x09) and len(payload) >= 4:
        text += " next=%ds" % struct.unpack_from("<H", payload, 2)[0]
    if (h["fcf"] == FCF_TAG_TO_AP) != (payload[0] in (0x01, 0x07, 0x0A)):
        text += " (unexpected direction)"
    return text


def describe(f, prev_us=None):
    """One frame as one line of text."""
    frame = f["frame"]
    dt = "" if prev_us is None else "+%.1fms" % ((f["us"] - prev_us) / 1000.0)
    head = "%12.6f %9s ch%-2d %4ddBm lqi%-3d %3dB " % (f["us"] / 1e6, dt, f["channel"], f["rssi"], f["lqi"], len(frame))
    h = decode_mac(frame)
    if h is None:
        return head + "short or odd frame: " + frame.hex().upper()
    kind = FRAME_TYPES.get(h["type"], "type%d" % h["type"])
    parts = [kind, "seq=%d" % h["seq"]]
    if h["src"] is not None:
        parts.append("%s/%04X" % (h["src"], h["src_pan"]) if h["src_pan"] is not None else h["src"])
    parts.append("->")
    parts.append("%s/%04X" % (h["dst"], h["dst_pan"]) if h["dst"] is not None else "-")
    if h["ack_req"]:
        parts.append("ackreq")
    if h["security"]:
        parts.append("secured")
    if h["version"]:
        parts.append("v%d" % h["version"])
    payload = frame[h["payload"]:]
    esl = decode_esl(h, payload)
    if esl:
        parts.append(esl)
    elif h["type"] == 1:
        parts.append("fcf=%04X" % h["fcf"])
    parts.append("| " + payload.hex().upper())
    return head + " ".join(parts)


def matches(f, pan=None, mac=None):
    """Whether a frame passes --pan and --mac (a frame whose header cannot be read passes neither)"""
    if pan is None and mac is None:
        return True
    h = decode_mac(f["frame"])
    if h is None:
        return False
    if pan is not None and pan not in (h["dst_pan"], h["src_pan"]):
        return False
    if mac is not None and mac not in (h["dst"], h["src"]):
        return False
    return True


class Pcap:
    """A capture file Wireshark opens: 802.15.4 without FCS (the sniffer never has it)"""

    def __init__(self, path):
        self.f = open(path, "wb")
        self.f.write(struct.pack("<IHHiIII", 0xA1B23C4D, 2, 4, 0, 0, 256, LINKTYPE_IEEE802_15_4_NOFCS))
        self.t0 = None

    def write(self, f):
        # the sniffer's clock counts from its start: place the frames from the first one's wall time
        if self.t0 is None:
            self.t0 = time.time_ns() - f["us"] * 1000
        ns = self.t0 + f["us"] * 1000
        data = f["frame"]
        self.f.write(struct.pack("<IIII", ns // 1000000000, ns % 1000000000, len(data), len(data)) + data)
        self.f.flush()

    def close(self):
        self.f.close()


class Port:
    """The sniffer's USB port, with DTR and RTS low (a raised pair restarts a C6)"""

    def __init__(self, path):
        self.ser = None
        try:
            import termios
            import fcntl
        except ImportError:
            import serial  # pyserial, on Windows
            self.ser = serial.Serial()
            self.ser.port, self.ser.baudrate, self.ser.timeout = path, 115200, 0.2
            self.ser.dtr = self.ser.rts = False
            self.ser.open()
            return
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        a = termios.tcgetattr(self.fd)
        a[0] = 0
        a[1] = 0
        a[2] = (a[2] | termios.CLOCAL | termios.CREAD) & ~termios.HUPCL
        a[3] = 0
        a[6][termios.VMIN], a[6][termios.VTIME] = 0, 2
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        fcntl.ioctl(self.fd, termios.TIOCMBIC, struct.pack("I", termios.TIOCM_DTR | termios.TIOCM_RTS))

    def read(self):
        return self.ser.read(4096) if self.ser else os.read(self.fd, 4096)

    def write(self, data):
        if self.ser:
            self.ser.write(data)
        else:
            os.write(self.fd, data)


def lines_from_port(port, commands):
    for c in commands:
        port.write((c + "\n").encode())
    buf = b""
    while True:
        chunk = port.read()
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            yield line.decode("utf-8", "replace").rstrip("\r")


def run(lines, args, out=sys.stdout):
    pcap = Pcap(args.pcap) if args.pcap else None
    log = open(args.log, "a", encoding="utf-8") if args.log else None
    prev = None
    n = 0
    try:
        for line in lines:
            if log:
                log.write(line + "\n")
                log.flush()
            f = parse_line(line)
            if f is None:
                at = line.find("@SNF-")
                if at >= 0:
                    print("# " + line[at:], file=out, flush=True)   # the sniffer's status and errors
                elif args.verbose and line.strip():
                    print("# " + line, file=out, flush=True)
                continue
            if not matches(f, args.pan, args.mac):
                continue
            print(describe(f, prev), file=out, flush=True)
            prev = f["us"]
            n += 1
            if pcap:
                pcap.write(f)
    except KeyboardInterrupt:
        pass
    finally:
        if pcap:
            pcap.close()
        if log:
            log.close()
    return n


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.strip().split("\n\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port", nargs="?", help="the sniffer's USB port (/dev/ttyACM0, COM5)")
    p.add_argument("--read", metavar="FILE", help="decode saved lines instead of reading a port")
    p.add_argument("--channel", type=int, choices=range(11, 27), metavar="11-26", help="tune the sniffer first")
    p.add_argument("--hop", type=int, metavar="MS", help="hop through 11-26, MS per channel (0 stops)")
    p.add_argument("--pan", type=lambda s: int(s, 0), help="only frames to or from this PAN (0x4553)")
    p.add_argument("--mac", type=lambda s: s.upper(), help="only frames to or from this address (16 hex digits, or 4 for a short one)")
    p.add_argument("--pcap", metavar="FILE", help="also write the frames to a capture for Wireshark")
    p.add_argument("--log", metavar="FILE", help="also keep the raw lines (decode them later with --read)")
    p.add_argument("-v", "--verbose", action="store_true", help="also print the AP's other log lines")
    args = p.parse_args(argv)
    if bool(args.port) == bool(args.read):
        p.error("give a port, or --read FILE")
    if args.read:
        if args.channel or args.hop is not None:
            p.error("--channel and --hop need a port")
        with open(args.read, encoding="utf-8", errors="replace") as fh:
            run((l.rstrip("\r\n") for l in fh), args)
        return 0
    commands = []
    if args.channel:
        commands.append("ch %d" % args.channel)
    if args.hop is not None:
        commands.append("hop %d" % args.hop)
    commands.append("status")
    run(lines_from_port(Port(args.port), commands), args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
