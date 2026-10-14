#!/usr/bin/env python3
"""
Label design in JSON (PC side). A design file says which layout a label uses,
which fields it fills, and which styles apply. It is run against data from the client and
gives the fields in the same form as the body of PUT /api/tags/{mac} (docs/protokoll.md).

The tag only knows the two layouts "price" and "text" (see src/layout.c). The design
therefore does not change the firmware; it is a PC tool that produces the content.

Design file (JSON; the // comments below are only explanations, not part of the file):
{
  "format": "esl-label",
  "version": 1,
  "id": "pris-standard",
  "layout": "price",                       // "price" or "text"
  "fields": {                              // price: name, price, subtitle
    "name": "{name}",                      // text: line1 .. line4
    "price": "{price}",
    "subtitle": "{unit_price}"
  },
  "style": { "price_red": true, "frame_red": false }
}

Values can contain placeholders like {key}. The client's data gives the value for each key.
Unknown placeholders, and text that cannot be written as Latin-1, are errors.

Usage:
  python tools/label_design.py check designs/*.json
  python tools/label_design.py render designs/pris-standard.json data.json
"""
import json
import re
import sys

FORMAT = "esl-label"
VERSION = 1

LAYOUTS = {
    "price": ["name", "price", "subtitle"],   # same order as the CONTENT packet
    "text": ["line1", "line2", "line3", "line4"],
}
STYLE_KEYS = {"price_red", "frame_red"}
MAX_TEXT_BYTES = 92        # ESL_MAX_TEXT_BYTES in shared/proto.h
MAX_FIELD_BYTES = 255      # the length is one byte in the packet
PLACEHOLDER = re.compile(r"\{([^{}]+)\}")
ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,39}$")


class DesignError(ValueError):
    pass


def _check_latin1(text, where):
    try:
        text.encode("latin-1")
    except UnicodeEncodeError as e:
        raise DesignError("%s: the character %r cannot be written as Latin-1" % (where, e.object[e.start]))


def validate(d):
    """Checks the structure. Placeholders are only checked in render()."""
    if not isinstance(d, dict):
        raise DesignError("the design must be a JSON object")
    if d.get("format") != FORMAT:
        raise DesignError("format must be %r" % FORMAT)
    if d.get("version") != VERSION:
        raise DesignError("only version %d is supported" % VERSION)
    if not isinstance(d.get("id"), str) or not ID_RE.match(d["id"]):
        raise DesignError("id must be lowercase letters, digits and hyphens (at most 40 characters)")
    layout = d.get("layout")
    if layout not in LAYOUTS:
        raise DesignError("layout must be one of %s" % ", ".join(sorted(LAYOUTS)))

    fields = d.get("fields")
    if not isinstance(fields, dict):
        raise DesignError("fields must be an object")
    expected = set(LAYOUTS[layout])
    unknown = set(fields) - expected
    if unknown:
        raise DesignError("unknown fields for layout %s: %s" % (layout, ", ".join(sorted(unknown))))
    for key in LAYOUTS[layout]:
        value = fields.get(key, "")
        if not isinstance(value, str):
            raise DesignError("field %s must be text" % key)
        _check_latin1(value, "field %s" % key)

    style = d.get("style", {})
    if not isinstance(style, dict):
        raise DesignError("style must be an object")
    unknown = set(style) - STYLE_KEYS
    if unknown:
        raise DesignError("unknown style: %s" % ", ".join(sorted(unknown)))
    for k, v in style.items():
        if not isinstance(v, bool):
            raise DesignError("style.%s must be true or false" % k)
    if layout == "text" and style.get("price_red"):
        raise DesignError("price_red only applies to layout price")
    return d


def _fill(template, data, where):
    def sub(m):
        key = m.group(1)
        if key not in data:
            raise DesignError("%s: missing data for {%s}" % (where, key))
        return str(data[key])
    return PLACEHOLDER.sub(sub, template)


def render(d, data):
    """
    Gives the fields for one piece of content, in the same form as PUT /api/tags/{mac}:
    {"layout": ..., "fields": [...], "price_red": bool, "frame_red": bool}
    Empty text gives an empty string. Text that cannot be written as Latin-1 is an error.
    """
    validate(d)
    layout = d["layout"]
    fields = []
    for key in LAYOUTS[layout]:
        value = _fill(d["fields"].get(key, ""), data, "%s.%s" % (d["id"], key))
        _check_latin1(value, "field %s" % key)
        if len(value.encode("latin-1")) > MAX_FIELD_BYTES:
            raise DesignError("field %s is too long" % key)
        fields.append(value)
    total = sum(len(f.encode("latin-1")) for f in fields)
    if total > MAX_TEXT_BYTES:
        raise DesignError("the text is %d bytes, maximum %d (requirement in protokoll.md)" % (total, MAX_TEXT_BYTES))
    style = d.get("style", {})
    out = {"layout": layout, "fields": fields}
    if layout == "price":
        out["price_red"] = bool(style.get("price_red", False))
    out["frame_red"] = bool(style.get("frame_red", False))
    return out


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def main(argv):
    if len(argv) >= 2 and argv[1] == "check" and len(argv) >= 3:
        bad = 0
        for p in argv[2:]:
            try:
                validate(load(p))
                print("OK   ", p)
            except (DesignError, ValueError) as e:
                bad += 1
                print("ERROR", p, "-", e)
        return 1 if bad else 0
    if len(argv) == 4 and argv[1] == "render":
        try:
            out = render(load(argv[2]), load(argv[3]))
        except (DesignError, ValueError) as e:
            print("ERROR", e)
            return 1
        print(json.dumps(out, ensure_ascii=False, indent=2))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
