"""Tests for tools/label_design.py. Run: python -m unittest discover -s test -p "test_label_*.py" """
import copy
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TAG = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(TAG, "tools"))

import label_design as ld  # noqa: E402

PRICE = {
    "format": "esl-label", "version": 1, "id": "pris-standard", "layout": "price",
    "fields": {"name": "{name}", "price": "{price}", "subtitle": "{unit_price}"},
    "style": {"price_red": True, "frame_red": False},
}
TEXT = {
    "format": "esl-label", "version": 1, "id": "tekst", "layout": "text",
    "fields": {"line1": "{a}", "line2": "", "line3": "", "line4": ""},
}


class ValidateTests(unittest.TestCase):
    def test_examples_are_valid(self):
        for name in ("pris-standard.json", "tekst-apningstider.json"):
            ld.validate(ld.load(os.path.join(TAG, "designs", name)))

    def test_rejects_bad_structure(self):
        cases = {
            "format": lambda d: d.update(format="other"),
            "version": lambda d: d.update(version=2),
            "id": lambda d: d.update(id="Bad id"),
            "layout": lambda d: d.update(layout="image"),
            "unknown field": lambda d: d["fields"].update(extra="x"),
            "unknown style": lambda d: d["style"].update(blink=True),
            "style not bool": lambda d: d["style"].update(price_red="yes"),
            "price_red on text": lambda d: (d.update(layout="text", fields={"line1": "x"})),
        }
        for label, change in cases.items():
            d = copy.deepcopy(PRICE)
            change(d)
            with self.subTest(label), self.assertRaises(ld.DesignError):
                ld.validate(d)

    def test_rejects_non_latin1_in_design(self):
        d = copy.deepcopy(PRICE)
        d["fields"]["subtitle"] = "kr/kg → 10"
        with self.assertRaises(ld.DesignError):
            ld.validate(d)


class RenderTests(unittest.TestCase):
    def test_price_output_matches_api_body(self):
        out = ld.render(PRICE, {"name": "Milk 1 L", "price": "24,90", "unit_price": "per l 24,90"})
        self.assertEqual(out, {
            "layout": "price",
            "fields": ["Milk 1 L", "24,90", "per l 24,90"],
            "price_red": True,
            "frame_red": False,
        })

    def test_text_has_four_fields_and_no_price_red(self):
        out = ld.render(TEXT, {"a": "Welcome"})
        self.assertEqual(out["fields"], ["Welcome", "", "", ""])
        self.assertNotIn("price_red", out)

    def test_missing_placeholder(self):
        with self.assertRaises(ld.DesignError):
            ld.render(PRICE, {"name": "x", "price": "1"})

    def test_norwegian_letters_kept(self):
        out = ld.render(PRICE, {"name": "Grøt æøå", "price": "9,90", "unit_price": ""})
        self.assertEqual(out["fields"][0], "Grøt æøå")

    def test_ap_replacements_allowed_and_counted(self):
        # The AP turns "–" into "-" and "…" into "..." (esl_text.c); the limit counts the result
        out = ld.render(PRICE, {"name": "Man–fre 9–21", "price": "9,90", "unit_price": "Mer…"})
        self.assertEqual(out["fields"][0], "Man–fre 9–21")
        self.assertEqual(ld.to_latin1("Mer…"), b"Mer...")
        self.assertEqual(ld.to_latin1("Søn – stengt"), "Søn - stengt".encode("latin-1"))
        d = copy.deepcopy(PRICE)
        d["fields"] = {"name": "A" * 60, "price": "B" * 20, "subtitle": "C" * 10 + "…"}
        with self.assertRaises(ld.DesignError):
            ld.render(d, {})  # 93 bytes after "…" becomes "..."

    def test_too_many_bytes(self):
        d = copy.deepcopy(PRICE)
        d["fields"] = {"name": "A" * 60, "price": "B" * 20, "subtitle": "C" * 20}
        with self.assertRaises(ld.DesignError):
            ld.render(d, {})
        d["fields"]["subtitle"] = "C" * 12
        ld.render(d, {})  # 92 bytes: exactly at the limit


if __name__ == "__main__":
    unittest.main()
