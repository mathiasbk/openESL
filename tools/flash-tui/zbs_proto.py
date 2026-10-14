#!/usr/bin/env python3
"""
Host side of the serial protocol spoken by atc1441's ZBS_Flasher firmware
(https://github.com/atc1441/ZBS_Flasher), here running on a D1 mini (ESP8266).

Written from scratch for OpenESL from a reading of the firmware (main.cpp) and the
reference client zbs_flasher.py. No code is copied.

Frame, both directions, 115200 8N1:

  'A' 'T' cmd len data[len] crc_hi crc_lo
  crc = (0xAB34 + cmd + len + sum(data)) & 0xFFFF

A command without data is sent with len = 0. The flasher answers every frame it accepts
with a frame carrying the same cmd. A frame with a bad CRC is silently dropped, and a frame
with a gap of more than 10 ms between bytes is thrown away.

Commands used here:

  GET_VERSION   1   -> 4 bytes, big endian
  ZBS_BEGIN    10   [slow_spi] -> [1] when the tag answers (RAM 0xBA write/read test).
                    Powers the tag and puts it in debug mode.
  RESET_ZBS    11   power cycle (500 ms) and let the tag run its firmware -> [1]
  SELECT_PAGE  12   [0 = main flash, 1 = info page] -> [1] if the SFR read back matches
  READ_FLASH   22   [len, addr_hi, addr_lo] -> len bytes (len <= 255)
  WRITE_FLASH  23   [len, addr_hi, addr_lo, data...] -> [1] ok, [0] failed, [0xEE] too long
                    (len must be < 252). Bytes equal to 0xFF are skipped, so the target
                    must be erased first. Each byte is read back and retried up to 4 times.
  ERASE_FLASH  26   erases the 64 KB main flash -> [1]

ERASE_INFOBLOCK (27) exists in the firmware but is deliberately not implemented here:
this tool never erases the info page.
"""
import struct
import time

import serial
import serial.tools.list_ports

BAUD = 115200

CMD_GET_VERSION = 1
CMD_ZBS_BEGIN = 10
CMD_RESET_ZBS = 11
CMD_SELECT_PAGE = 12
CMD_READ_FLASH = 22
CMD_WRITE_FLASH = 23
CMD_ERASE_FLASH = 26

PAGE_MAIN = 0
PAGE_INFO = 1

FLASH_SIZE = 0x10000
INFO_SIZE = 0x400
READ_CHUNK = 0xFF           # largest READ_FLASH length (one length byte)
WRITE_CHUNK = 250           # firmware refuses len >= 252

CH340_VID = 0x1A86

FRAME_HEAD = b"AT"
CRC_INIT = 0xAB34


class FlasherError(Exception):
    """Talking to the flasher failed (timeout, CRC, wrong answer)."""


class NoTagError(FlasherError):
    """The flasher answers, but no tag answers on the debug interface."""


def checksum(body):
    return (CRC_INIT + sum(body)) & 0xFFFF


def encode_frame(cmd, data=b""):
    if len(data) > 255:
        raise ValueError("payload too long: %d" % len(data))
    body = bytes([cmd & 0xFF, len(data)]) + bytes(data)
    return FRAME_HEAD + body + checksum(body).to_bytes(2, "big")


class ZbsFlasher:
    """One ZBS_Flasher on an open serial port (or anything with the same methods)."""

    def __init__(self, port, retries=3, slow_spi=False):
        self.port = port
        self.retries = retries
        self.slow_spi = slow_spi

    # ---- framing ----

    def flush_input(self):
        """Throw away anything waiting, e.g. a stray second answer or ESP boot noise."""
        self.port.reset_input_buffer()
        time.sleep(0.02)
        while self.port.in_waiting:
            self.port.read(self.port.in_waiting)

    def _read_frame(self):
        junk = self.port.read_until(FRAME_HEAD)
        if not junk.endswith(FRAME_HEAD):
            raise FlasherError("timeout waiting for an answer")
        head = self.port.read(2)
        if len(head) != 2:
            raise FlasherError("timeout in answer header")
        cmd, n = head[0], head[1]
        data = self.port.read(n) if n else b""
        if len(data) != n:
            raise FlasherError("timeout in answer data (%d of %d bytes)" % (len(data), n))
        crc = self.port.read(2)
        if len(crc) != 2:
            raise FlasherError("timeout in answer CRC")
        if int.from_bytes(crc, "big") != checksum(head + data):
            raise FlasherError("CRC error in answer")
        return cmd, data

    def query(self, cmd, data=b"", retries=None):
        """Send one command and return the answer's data. Retries on timeout/CRC errors."""
        tries = self.retries if retries is None else retries
        last = None
        for _ in range(max(1, tries)):
            try:
                self.port.write(encode_frame(cmd, data))
                rcmd, rdata = self._read_frame()
                if rcmd != cmd:
                    raise FlasherError("answer for command %d, expected %d" % (rcmd, cmd))
                return rdata
            except FlasherError as e:
                last = e
                self.flush_input()
        raise FlasherError("command %d failed after %d tries: %s" % (cmd, tries, last))

    def _status(self, cmd, data=b""):
        r = self.query(cmd, data)
        if len(r) != 1:
            raise FlasherError("command %d: expected 1 status byte, got %d" % (cmd, len(r)))
        return r[0]

    # ---- commands ----

    def version(self, retries=None):
        r = self.query(CMD_GET_VERSION, retries=retries)
        if len(r) != 4:
            raise FlasherError("version answer has %d bytes, expected 4" % len(r))
        return struct.unpack(">I", r)[0]

    def begin(self):
        """Power the tag and enter debug mode. Raises NoTagError when no tag answers."""
        if self._status(CMD_ZBS_BEGIN, bytes([1 if self.slow_spi else 0])) != 1:
            raise NoTagError("no contact with the tag (check that it sits right in the jig)")

    def tag_present(self):
        try:
            self.begin()
            return True
        except NoTagError:
            return False

    def reset(self):
        self.query(CMD_RESET_ZBS)

    def select_page(self, page):
        if self._status(CMD_SELECT_PAGE, bytes([1 if page else 0])) != 1:
            raise FlasherError("could not select flash page %d (tag contact?)" % page)

    def read_chunk(self, addr, length):
        if not 0 < length <= READ_CHUNK:
            raise ValueError("read length %d" % length)
        if addr < 0 or addr + length > FLASH_SIZE:
            raise ValueError("read 0x%04X+%d out of range" % (addr, length))
        r = self.query(CMD_READ_FLASH, bytes([length, addr >> 8, addr & 0xFF]))
        if len(r) != length:
            raise FlasherError("read 0x%04X: got %d of %d bytes" % (addr, len(r), length))
        return r

    def read(self, addr, length, progress=None):
        """Read from the selected page. progress(done, total) is called per chunk."""
        out = bytearray()
        while len(out) < length:
            n = min(READ_CHUNK, length - len(out))
            out += self.read_chunk(addr + len(out), n)
            if progress:
                progress(len(out), length)
        return bytes(out)

    def write_chunk(self, addr, data):
        if not 0 < len(data) <= WRITE_CHUNK:
            raise ValueError("write length %d" % len(data))
        if addr < 0 or addr + len(data) > FLASH_SIZE:
            raise ValueError("write 0x%04X+%d out of range" % (addr, len(data)))
        r = self.query(CMD_WRITE_FLASH, bytes([len(data), addr >> 8, addr & 0xFF]) + bytes(data))
        if r != b"\x01":
            # On a failed byte the firmware sends [0] and then also [1]; drop the extra answer.
            self.flush_input()
            raise FlasherError("write failed at 0x%04X (answer %s)" % (addr, r.hex()))

    def write(self, addr, data, progress=None):
        """Write to the selected (already erased) page. Chunks of only 0xFF are skipped."""
        done = 0
        while done < len(data):
            chunk = data[done:done + WRITE_CHUNK]
            if chunk.count(0xFF) != len(chunk):
                self.write_chunk(addr + done, chunk)
            done += len(chunk)
            if progress:
                progress(done, len(data))

    def erase_main(self):
        """Erase the 64 KB main flash. Selects the main page first, so the info page is never hit."""
        self.select_page(PAGE_MAIN)
        if self._status(CMD_ERASE_FLASH) != 1:
            raise FlasherError("erase failed")


# ---- ports ----

def list_ports():
    """All serial ports, as dicts with device, location (USB path), vid/pid and description."""
    out = []
    for p in serial.tools.list_ports.comports():
        out.append({
            "device": p.device,
            "location": p.location or "",
            "vid": p.vid,
            "pid": p.pid,
            "serial": p.serial_number or "",
            "description": p.description or "",
            "ch340": p.vid == CH340_VID,
        })
    return sorted(out, key=lambda p: p["device"])


def open_port(device, timeout=2.0):
    """Open a real serial port for the flasher.

    DTR and RTS are set low before opening, so the D1 mini's auto-reset circuit is not
    pulsed on purpose. If the board resets anyway, probe() waits for it to boot.
    """
    s = serial.Serial()
    s.port = device
    s.baudrate = BAUD
    s.timeout = timeout
    s.dtr = False
    s.rts = False
    s.open()
    return s


def probe(flasher, wait_s=3.0):
    """Check that the port really is a ZBS_Flasher. Returns the version or raises FlasherError.

    Only GET_VERSION is sent: it does not touch the tag. Repeats for up to wait_s seconds,
    because an ESP8266 that was reset by opening the port needs about a second to boot.
    """
    end = time.monotonic() + wait_s
    last = None
    while True:
        try:
            flasher.flush_input()
            return flasher.version(retries=1)
        except FlasherError as e:
            last = e
        if time.monotonic() >= end:
            raise FlasherError("the port does not answer the ZBS_Flasher protocol (%s). "
                               "Wrong port, e.g. the debug Wemos?" % last)
        time.sleep(0.2)
