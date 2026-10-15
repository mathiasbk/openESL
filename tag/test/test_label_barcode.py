"""Reads back the barcodes test_barcode.c drew (build/host/barcodes/*.pbm) with a real
decoder, so the bars on the screen are checked against the standards and not only against
the encoder itself. Needs zxing-cpp (pip install zxing-cpp) and Pillow; skipped without them.
CI installs zxing-cpp. The tag's 1-pixel modules are scaled up 3 times, as a camera would
see a sharp label, and a white border is added for the quiet zone at the screen edge."""
import glob
import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.environ.get("ESL_TEST_OUT", os.path.join(HERE, "..", "build", "host", "barcodes"))

try:
    import zxingcpp
    from PIL import Image, ImageOps
except ImportError:  # pragma: no cover
    zxingcpp = None

FORMATS = {"ean13": "EAN13", "code128": "Code128"}


@unittest.skipIf(zxingcpp is None, "zxing-cpp is not installed")
class DecodeTest(unittest.TestCase):
    def test_decode(self):
        lists = glob.glob(os.path.join(OUT, "*-list.txt"))
        if not lists:
            self.skipTest("run the C tests first (python tools/build.py test)")
        n = 0
        for lst in lists:
            with open(lst, encoding="ascii") as f:
                for line in f:
                    name, sym, text = line.rstrip("\n").split("\t")
                    with self.subTest(name):
                        img = Image.open(os.path.join(OUT, name)).convert("L")
                        img = ImageOps.expand(img, border=20, fill=255)
                        img = img.resize((img.width * 3, img.height * 3), Image.NEAREST)
                        res = zxingcpp.read_barcodes(img)
                        self.assertEqual(len(res), 1, "found %d codes" % len(res))
                        self.assertEqual(res[0].format.name, FORMATS[sym])
                        self.assertEqual(res[0].text, text)
                        n += 1
        self.assertGreater(n, 0)


if __name__ == "__main__":
    unittest.main()
