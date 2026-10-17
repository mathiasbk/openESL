#!/usr/bin/env python3
"""
Label design in JSON (PC side). A design file says which layout a label uses,
which fields it fills, and which styles apply. It is run against data from the client and
gives the fields in the same form as the body of PUT /api/tags/{mac} (docs/protokoll.md).

The layout is either one of the tag's built-in layouts, "price" and "text" (src/layout.c),
or the name of a layout in a layout file (designs/layouts/<model>.json, uploaded with
PUT /api/layouts/<model>, docs/protokoll.md part 3). The design does not change the
firmware; it is a PC tool that produces the content.

Design file (JSON; the // comments below are only explanations, not part of the file):
{
  "format": "esl-label",
  "version": 1,
  "id": "pris-standard",
  "layout": "price",                       // "price", "text", or a layout name from a layout file
  "fields": {                              // price: name, price, subtitle
    "name": "{name}",                      // text: line1 .. line4
    "price": "{price}",                    // a file layout: the names in its "fields"
    "subtitle": "{unit_price}"
  },
  "style": { "price_highlight": true, "frame": false }
}                                          // price_highlight: white price in a black box
                                           // (price layout, and "highlight" elements of a file
                                           // layout); frame: black frame. The tags are
                                           // black/white, there is no red.

Layout file (format "esl-layouts"): see docs/protokoll.md part 3. check_layout_file() only
checks the outline here (names and fields); the AP checks positions, fonts and sizes against
the model when the file is uploaded, and tools/render_host.py runs the AP's own check.

Values can contain placeholders like {key}. The client's data gives the value for each key.
Unknown placeholders, and text that cannot be written as Latin-1, are errors. The characters
the AP itself replaces (dashes, curly quotes, special spaces, "…"; see AP_REPLACE) are allowed,
and the text limit is counted after that replacement, as the AP does.

Usage:
  python tools/label_design.py check designs/*.json designs/layouts/*.json
  python tools/label_design.py render designs/pris-standard.json data.json
  python tools/label_design.py render designs/hylle-tilbud.json data.json designs/layouts/2.9.json
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "shared"))
import models  # noqa: E402  (the model registry, shared/models.json)

FORMAT = "esl-label"
VERSION = 1

LAYOUTS = {
    "price": ["name", "price", "subtitle"],   # same order as the CONTENT packet
    "text": ["line1", "line2", "line3", "line4"],
}
STYLE_KEYS = {"price_highlight", "frame"}
LAYOUT_FILE_FORMAT = "esl-layouts"
LAYOUT_NAME_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,30}$")    # esl_layout_name_valid on the AP
FIELD_NAME_RE = re.compile(r"^[a-z][a-z0-9_]{0,15}$")
MODELS = tuple(m["name"] for m in models.load())   # as in the API and the layout files
MAX_FIELDS = 6             # ESL_MAX_FIELDS in shared/proto.h
FIELD_SPACE = 96           # ESL_CONTENT_FIELD_SPACE: text + one length byte per field sent
MAX_FIELD_BYTES = 255      # the length is one byte in the packet
PLACEHOLDER = re.compile(r"\{([^{}]+)\}")
ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,39}$")


# Characters outside Latin-1 that the AP replaces before sending
# (ap/components/esl_core/esl_text.c). Keep the two in step.
AP_REPLACE = {}
AP_REPLACE.update({c: " " for c in "\u2002\u2003\u2009\u200a\u202f"})
AP_REPLACE.update({c: "-" for c in "\u2010\u2011\u2012\u2013\u2014\u2015\u2212"})
AP_REPLACE.update({c: "'" for c in "\u2018\u2019\u201a\u2032"})
AP_REPLACE.update({c: '"' for c in "\u201c\u201d\u201e\u2033"})
AP_REPLACE["\u2022"] = "\u00b7"
AP_REPLACE["\u2026"] = "..."


class DesignError(ValueError):
    pass


def to_latin1(text):
    """The bytes the tag receives for text (after the AP's replacements)."""
    return "".join(AP_REPLACE.get(c, c) for c in text).replace("\u00a0", " ").encode("latin-1")


def _check_latin1(text, where):
    try:
        to_latin1(text)
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
    if not isinstance(layout, str) or (layout not in LAYOUTS and not LAYOUT_NAME_RE.match(layout)):
        raise DesignError("layout must be price, text or the name of a layout in a layout file "
                          "(a-z 0-9 . _ -, at most 31 characters)")

    fields = d.get("fields")
    if not isinstance(fields, dict):
        raise DesignError("fields must be an object")
    if layout in LAYOUTS:
        unknown = set(fields) - set(LAYOUTS[layout])
        if unknown:
            raise DesignError("unknown fields for layout %s: %s" % (layout, ", ".join(sorted(unknown))))
    for key, value in fields.items():
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
    if layout == "text" and style.get("price_highlight"):
        raise DesignError("price_highlight does not apply to layout text")
    return d


def _fill(template, data, where):
    def sub(m):
        key = m.group(1)
        if key not in data:
            raise DesignError("%s: missing data for {%s}" % (where, key))
        return str(data[key])
    return PLACEHOLDER.sub(sub, template)


def check_layout_file(f):
    """Outline of a layout file: format, model, unique layout names and their field names."""
    if not isinstance(f, dict) or f.get("format") != LAYOUT_FILE_FORMAT:
        raise DesignError("format must be %r" % LAYOUT_FILE_FORMAT)
    if f.get("version") != 1:
        raise DesignError("only version 1 is supported")
    if f.get("model") not in MODELS:
        raise DesignError("model must be one of %s" % ", ".join(MODELS))
    layouts = f.get("layouts")
    if not isinstance(layouts, list) or not 1 <= len(layouts) <= 16:
        raise DesignError("layouts must be a list of 1 to 16 layouts")
    names = set()
    for i, lay in enumerate(layouts):
        name = lay.get("name") if isinstance(lay, dict) else None
        if not isinstance(name, str) or not LAYOUT_NAME_RE.match(name) or name in LAYOUTS:
            raise DesignError("layouts[%d]: bad name %r" % (i, name))
        if name in names:
            raise DesignError("layouts[%d]: the name %r is used twice" % (i, name))
        names.add(name)
        flds = lay.get("fields")
        if (not isinstance(flds, list) or not 1 <= len(flds) <= MAX_FIELDS or len(set(flds)) != len(flds)
                or not all(isinstance(x, str) and FIELD_NAME_RE.match(x) for x in flds)):
            raise DesignError("layouts[%d]: fields must list 1 to %d field names" % (i, MAX_FIELDS))
        if not isinstance(lay.get("elements"), list) or not lay["elements"]:
            raise DesignError("layouts[%d]: elements must be a list of at least one element" % i)
    return f


def layout_fields(layout, layout_file=None):
    """The field names of a layout, in the order they are sent. None if the file lacks it."""
    if layout in LAYOUTS:
        return LAYOUTS[layout]
    for lay in (layout_file or {}).get("layouts", []):
        if lay.get("name") == layout:
            return lay["fields"]
    return None


def render(d, data, layout_file=None):
    """
    Gives the fields for one piece of content, in the same form as PUT /api/tags/{mac}:
    {"layout": ..., "fields": [...], "price_highlight": bool, "frame": bool}
    Empty text gives an empty string. Text that cannot be written as Latin-1 is an error.
    A design for a layout from a layout file needs that file (layout_file, parsed JSON),
    for the order of the fields.
    """
    validate(d)
    layout = d["layout"]
    order = layout_fields(layout, layout_file)
    if order is None:
        raise DesignError("layout %r is not in the layout file%s" % (
            layout, " for model %s" % layout_file.get("model") if layout_file else " (none given)"))
    unknown = set(d["fields"]) - set(order)
    if unknown:
        raise DesignError("unknown fields for layout %s: %s" % (layout, ", ".join(sorted(unknown))))
    fields = []
    for key in order:
        value = _fill(d["fields"].get(key, ""), data, "%s.%s" % (d["id"], key))
        _check_latin1(value, "field %s" % key)
        if len(to_latin1(value)) > MAX_FIELD_BYTES:
            raise DesignError("field %s is too long" % key)
        fields.append(value)
    sent = len(fields)
    while sent and not fields[sent - 1]:
        sent -= 1                       # the AP drops empty fields at the end
    total = sum(len(to_latin1(f)) for f in fields)
    if total + sent > FIELD_SPACE:
        raise DesignError("the text is %d bytes, maximum %d with %d fields (requirement in protokoll.md)"
                          % (total, FIELD_SPACE - sent, sent))
    style = d.get("style", {})
    out = {"layout": layout, "fields": fields}
    if layout != "text":
        out["price_highlight"] = bool(style.get("price_highlight", False))
    out["frame"] = bool(style.get("frame", False))
    return out


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def main(argv):
    if len(argv) >= 2 and argv[1] == "check" and len(argv) >= 3:
        bad = 0
        for p in argv[2:]:
            try:
                d = load(p)
                if isinstance(d, dict) and d.get("format") == LAYOUT_FILE_FORMAT:
                    check_layout_file(d)
                    print("OK   ", p, "(outline only; the AP checks positions and fonts)")
                    continue
                validate(d)
                print("OK   ", p)
            except (DesignError, ValueError) as e:
                bad += 1
                print("ERROR", p, "-", e)
        return 1 if bad else 0
    if len(argv) in (4, 5) and argv[1] == "render":
        try:
            out = render(load(argv[2]), load(argv[3]), load(argv[4]) if len(argv) == 5 else None)
        except (DesignError, ValueError) as e:
            print("ERROR", e)
            return 1
        print(json.dumps(out, ensure_ascii=False, indent=2))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
