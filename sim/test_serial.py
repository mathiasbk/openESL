#!/usr/bin/env python3
"""The whole chain without hardware: tools/esl-client over the AP's serial framing to
"esl-sim serve", which runs the AP core and a simulated tag on a pseudo-terminal.

  python3 test_serial.py          (after make; ESL_SIM=path picks another build)

The tag polls every 60 simulated seconds; the simulator runs 120 times faster, so a
poll comes every half second.
"""
import os
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CLIENT = os.path.join(ROOT, "tools", "esl-client", "esl_client.py")
sys.path.insert(0, os.path.dirname(CLIENT))
import esl_client  # noqa: E402

SIM = os.environ.get("ESL_SIM", os.path.join(HERE, "build", "154", "esl-sim"))
MAC = "00000123456789A8"


class SerialChain(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.png = os.path.join(cls.tmp.name, "screen.png")
        cls.sim = subprocess.Popen([SIM, "serve", "--speed", "120", "--png", cls.png],
                                   stdout=subprocess.PIPE, text=True)
        first = cls.sim.stdout.readline().split()
        assert first[:1] == ["PTY"], first
        cls.port = first[1]
        cls.cli = esl_client.SerialClient(cls.port, timeout=5)

    @classmethod
    def tearDownClass(cls):
        cls.sim.kill()
        cls.sim.wait()
        cls.sim.stdout.close()
        if cls.cli.fd is not None:
            os.close(cls.cli.fd)
        cls.tmp.cleanup()

    def wait_for(self, what, cond, timeout_s=10):
        end = time.monotonic() + timeout_s
        while time.monotonic() < end:
            v = cond()
            if v:
                return v
            time.sleep(0.1)
        self.fail("timed out waiting for " + what)

    def test_1_tag_appears(self):
        tags = self.wait_for("the tag's first POLL",
                             lambda: [t for t in self.cli.tags()["tags"] if t["mac"] == MAC])
        self.assertEqual(tags[0]["model"], "1.54")

    def test_2_price_shown(self):
        r = self.cli.set_tag(MAC, esl_client.content("price", ["Lettmelk 1 L", "24,90", "kr/l 24,90"]))
        self.assertTrue(r["pending"])
        self.assertTrue(self.cli.wait_shown(MAC, r["content_id"], timeout_s=10, poll_s=0.1))
        self.assertFalse(self.cli.tag(MAC)["pending"])
        with open(self.png, "rb") as f:
            self.assertEqual(f.read(8), b"\x89PNG\r\n\x1a\n")

    def test_3_cli(self):
        """The command line client as a user runs it."""
        out = subprocess.run([sys.executable, CLIENT, "--serial", self.port, "status"],
                             capture_output=True, text=True, timeout=30)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn(MAC, out.stdout)
        out = subprocess.run([sys.executable, CLIENT, "--serial", self.port, "text", MAC,
                              "Velkommen", "Man-fre 9-21", "--frame", "--wait", "--wait-s", "20"],
                             capture_output=True, text=True, timeout=60)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
