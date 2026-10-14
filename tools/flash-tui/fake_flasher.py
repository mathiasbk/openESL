#!/usr/bin/env python3
"""
A simulated ZBS_Flasher on a fake serial port, for tests and for the TUI's --fake mode.

FakeTag holds a 64 KB main flash and a 1 KB info page that behave like NOR flash:
erasing sets 0xFF, writing can only clear bits. FakeFlasherPort parses the same frames as
the real firmware (main.cpp) and answers them, including its quirks:

  - WRITE_FLASH skips 0xFF bytes and reads every byte back (4 tries);
  - on a failed byte it answers [0] and then also [1] (the firmware's `break` only leaves
    the inner loop);
  - frames with a bad CRC get no answer.

Nothing here opens a real device.
"""
import collections
import random

import zbs_proto as zp

FLASHER_VERSION = 0x20


def stock_tag(model="29", seed=1):
    """A tag with a made-up stock firmware: MAC bytes at 0xFC06, blank info page MAC."""
    rnd = random.Random(seed)
    flash = bytearray(rnd.randrange(256) for _ in range(0xC000)) + bytearray([0xFF] * 0x4000)
    flash[0xFC00:0xFC10] = bytes([0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x02, 0x07, 0x93, 0x50 + seed % 16, 0x01, 0x02,
                                  0xFF, 0xFF, 0xFF, 0xFF])
    info = bytearray([0xFF] * zp.INFO_SIZE)
    info[0:0x10] = bytes.fromhex("0c1721080109000001183a029a032b18")
    return FakeTag(bytes(flash), bytes(info))


class FakeTag:
    def __init__(self, flash=None, info=None):
        self.pages = [bytearray(flash or b"\xff" * zp.FLASH_SIZE),
                      bytearray(info or b"\xff" * zp.INFO_SIZE)]
        assert len(self.pages[0]) == zp.FLASH_SIZE and len(self.pages[1]) == zp.INFO_SIZE
        self.page = 0
        self.erase_count = [0, 0]
        self.writes = [0, 0]
        self.stuck_bits = {}            # (page, addr) -> mask of bits that never clear
        self.read_corrupt = None        # function(page, addr, value) -> value

    @property
    def flash(self):
        return self.pages[0]

    @property
    def info(self):
        return self.pages[1]

    def read(self, addr):
        page = self.pages[self.page]
        v = page[addr % len(page)]
        if self.read_corrupt:
            v = self.read_corrupt(self.page, addr, v)
        return v

    def write(self, addr, value):
        page = self.pages[self.page]
        a = addr % len(page)
        page[a] &= value | self.stuck_bits.get((self.page, a), 0)
        self.writes[self.page] += 1

    def erase(self, page):
        self.pages[page][:] = b"\xff" * len(self.pages[page])
        self.erase_count[page] += 1


class FakeFlasherPort:
    """Looks like a pyserial Serial object with a ZBS_Flasher on the other end."""

    def __init__(self, tag=None, timeout=0.05):
        self.tag = tag
        self.timeout = timeout
        self.is_open = True
        self.rx = collections.deque()       # bytes on their way to the host
        self._buf = bytearray()             # bytes from the host, not parsed yet
        self.commands = []                  # every command received, in order
        self.powered = False
        self.drop_next = 0                  # answers to swallow (simulates lost frames)
        self.corrupt_next = 0               # answers to send with a bad CRC
        self.noise = b""                    # sent before the next answer

    # ---- pyserial surface ----

    @property
    def in_waiting(self):
        return len(self.rx)

    def reset_input_buffer(self):
        self.rx.clear()

    def flush(self):
        pass

    def close(self):
        self.is_open = False

    def write(self, data):
        self._buf += data
        self._parse()
        return len(data)

    def read(self, n=1):
        out = bytearray()
        while self.rx and len(out) < n:
            out.append(self.rx.popleft())
        return bytes(out)

    def read_until(self, expected=b"\n", size=None):
        out = bytearray()
        while self.rx:
            out.append(self.rx.popleft())
            if out.endswith(expected):
                break
        return bytes(out)

    # ---- device side ----

    def _parse(self):
        while True:
            i = self._buf.find(zp.FRAME_HEAD)
            if i < 0:
                self._buf = self._buf[-1:] if self._buf.endswith(b"A") else bytearray()
                return
            del self._buf[:i]
            if len(self._buf) < 4:
                return
            n = self._buf[3]
            if len(self._buf) < 4 + n + 2:
                return
            body = bytes(self._buf[2:4 + n])
            crc = int.from_bytes(self._buf[4 + n:6 + n], "big")
            del self._buf[:6 + n]
            if crc == zp.checksum(body):
                self._handle(body[0], body[2:])

    def _answer(self, cmd, data=b""):
        if self.drop_next:
            self.drop_next -= 1
            return
        frame = bytearray(zp.encode_frame(cmd, data))
        if self.corrupt_next:
            self.corrupt_next -= 1
            frame[-1] ^= 0x55
        self.rx.extend(self.noise + bytes(frame))
        self.noise = b""

    def _handle(self, cmd, data):
        self.commands.append(cmd)
        tag = self.tag if self.powered else None
        if cmd == zp.CMD_GET_VERSION:
            self._answer(cmd, FLASHER_VERSION.to_bytes(4, "big"))
        elif cmd == zp.CMD_ZBS_BEGIN:
            self.powered = True
            self._answer(cmd, bytes([1 if self.tag is not None else 0]))
        elif cmd == zp.CMD_RESET_ZBS:
            self.powered = self.tag is not None
            if self.tag:
                self.tag.page = 0
            self._answer(cmd, b"\x01")
        elif cmd == zp.CMD_SELECT_PAGE:
            ok = tag is not None
            if ok:
                tag.page = 1 if data[0] else 0
            self._answer(cmd, bytes([1 if ok else 0]))
        elif cmd == zp.CMD_READ_FLASH:
            n, addr = data[0], (data[1] << 8) | data[2]
            out = bytes(tag.read(addr + i) if tag else 0xFF for i in range(n))
            self._answer(cmd, out)
        elif cmd == zp.CMD_WRITE_FLASH:
            n, addr = data[0], (data[1] << 8) | data[2]
            if n >= 0xFF - 3:
                self._answer(cmd, b"\xee")
                return
            for i in range(n):
                v = data[3 + i]
                if v == 0xFF:
                    continue
                ok = False
                for _ in range(4):
                    if tag:
                        tag.write(addr + i, v)
                        if tag.read(addr + i) == v:
                            ok = True
                            break
                if not ok:
                    self._answer(cmd, b"\x00")
                    break
            self._answer(cmd, b"\x01")
        elif cmd == zp.CMD_ERASE_FLASH:
            if tag:
                tag.erase(0)
            self._answer(cmd, b"\x01")
        elif cmd == 27:   # ERASE_INFOBLOCK: the real firmware does it; we record it so tests can fail
            if tag:
                tag.erase(1)
            self._answer(cmd, b"\x01")
        # unknown commands: the real firmware sends nothing


class FakeDebugWemosPort(FakeFlasherPort):
    """Not a flasher: prints text and ignores everything it gets (like tools/wemos-debug)."""

    def _parse(self):
        self._buf.clear()
        self.rx.extend(b"[wemos-debug] listening on D5 at 115200 baud\r\nAT boot\r\n")
