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
    "style": {"price_highlight": True, "frame": False},
}
TEXT = {
    "format": "esl-label", "version": 1, "id": "tekst", "layout": "text",
    "fields": {"line1": "{a}", "line2": "", "line3": "", "line4": ""},
}


class ValidateTests(unittest.TestCase):
    def test_examples_are_valid(self):
        for name in ("pris-standard.json", "tekst-apningstider.json", "hylle-tilbud.json",
                     "skilt-velkommen.json"):
            ld.validate(ld.load(os.path.join(TAG, "designs", name)))

    def test_rejects_bad_structure(self):
        cases = {
            "format": lambda d: d.update(format="other"),
            "version": lambda d: d.update(version=2),
            "id": lambda d: d.update(id="Bad id"),
            "layout": lambda d: d.update(layout="Bad name"),
            "layout reserved": lambda d: d.update(layout=5),
            "unknown field": lambda d: d["fields"].update(extra="x"),
            "unknown style": lambda d: d["style"].update(blink=True),
            "style not bool": lambda d: d["style"].update(price_highlight="yes"),
            "price_highlight on text": lambda d: (d.update(layout="text", fields={"line1": "x"})),
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
            "price_highlight": True,
            "frame": False,
        })

    def test_text_has_four_fields_and_no_price_highlight(self):
        out = ld.render(TEXT, {"a": "Welcome"})
        self.assertEqual(out["fields"], ["Welcome", "", "", ""])
        self.assertNotIn("price_highlight", out)

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


class LayoutFileTests(unittest.TestCase):
    """Designs for layouts from a layout file (docs/protokoll.md part 3)."""

    def setUp(self):
        self.lf = {m: ld.load(os.path.join(TAG, "designs", "layouts", m + ".json")) for m in ("2.9", "1.54")}
        self.shelf = ld.load(os.path.join(TAG, "designs", "hylle-tilbud.json"))

    def test_example_files_outline(self):
        for f in self.lf.values():
            ld.check_layout_file(f)
            self.assertEqual([lay["name"] for lay in f["layouts"]], ["shelf", "sign"])

    def test_render_uses_the_file_field_order(self):
        data = {"name": "Kaffe", "price": "49,90", "unit_price": "kr 99,80/kg", "note": "Tilbud"}
        for f in self.lf.values():
            out = ld.render(self.shelf, data, f)
            self.assertEqual(out, {"layout": "shelf", "fields": ["Kaffe", "49,90", "kr 99,80/kg", "Tilbud"],
                                   "price_highlight": True, "frame": False})

    def test_render_needs_the_layout(self):
        data = {"name": "a", "price": "1", "unit_price": "", "note": ""}
        with self.assertRaises(ld.DesignError):
            ld.render(self.shelf, data)
        f = copy.deepcopy(self.lf["2.9"])
        f["layouts"][0]["name"] = "other"
        with self.assertRaises(ld.DesignError):
            ld.render(self.shelf, data, f)

    def test_unknown_field_for_file_layout(self):
        d = copy.deepcopy(self.shelf)
        d["fields"]["subtitle"] = "x"
        with self.assertRaises(ld.DesignError):
            ld.render(d, {"name": "a", "price": "1", "unit_price": "", "note": ""}, self.lf["2.9"])

    def test_bad_layout_files(self):
        cases = {
            "format": lambda f: f.update(format="esl-label"),
            "model": lambda f: f.update(model="4.2"),
            "no layouts": lambda f: f.update(layouts=[]),
            "reserved name": lambda f: f["layouts"][0].update(name="price"),
            "duplicate name": lambda f: f["layouts"][1].update(name="shelf"),
            "five fields": lambda f: f["layouts"][0].update(fields=["a", "b", "c", "d", "e"]),
            "bad field": lambda f: f["layouts"][0].update(fields=["Name"]),
            "no elements": lambda f: f["layouts"][0].update(elements=[]),
        }
        for label, change in cases.items():
            f = copy.deepcopy(self.lf["2.9"])
            change(f)
            with self.subTest(label), self.assertRaises(ld.DesignError):
                ld.check_layout_file(f)
