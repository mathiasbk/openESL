"""Shared test setup: import path, and a guard so no test can open a real serial port."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import serial  # noqa: E402


class _NoRealPorts(serial.Serial):
    def open(self):
        raise AssertionError("a test tried to open a real serial port: %r" % self.port)


serial.Serial = _NoRealPorts
