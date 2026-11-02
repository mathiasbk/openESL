#!/usr/bin/env python3
"""
Checks that the stylesheets of the web page (web/css/*.css) keep to the design scale in the
:root block of web/css/style.css (#317), so the page looks the same everywhere and a new rule
does not break dark mode:

  - no colour outside a :root block: no #hex, rgb(), rgba(), hsl(), hsla() or colour name.
    Use a variable (var(--text), var(--muted), ...) or color-mix() of variables; add a new
    variable to :root, with its dark value in both dark blocks, when none fits.
  - no font size in px, pt or rem outside a :root block: use var(--fs-xs) ... var(--fs-2xl).

  python tools/check_css.py [file.css ...]     default: web/css/*.css

tools/check.py runs it in the web step. Exit code: 1 when a rule breaks the scale, otherwise 0.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COLOUR = re.compile(r"#[0-9a-fA-F]{3,8}(?![\w-])|\b(?:rgba?|hsla?)\(")
NAMED = re.compile(r"\b(?:white|black|red|green|blue|gr[ae]y|orange|yellow|purple|pink|silver|navy|maroon)\b", re.I)
COLOUR_PROPS = re.compile(r"^(?:color|background(?:-color)?|border(?:-[a-z]+)*|outline(?:-color)?|fill|stroke|"
                          r"box-shadow|text-shadow|caret-color|accent-color|text-decoration(?:-color)?|--[\w-]+)$")
FONT_SIZE = re.compile(r"(?<![\w-])\d*\.?\d+(?:px|pt|rem)\b")


def strip_comments(css):
    # keep the newlines so the line numbers stay right
    return re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), css, flags=re.S)


def check(path):
    """[(line, message)] for one stylesheet"""
    css = strip_comments(open(path, encoding="utf-8").read())
    problems = []
    stack = []          # the selector (or at-rule) of each open block
    start = 0           # where the text since the last { ; } starts
    line = 1
    for i, ch in enumerate(css):
        if ch not in "{};":
            continue
        text = css[start:i]
        at = line + text[:len(text) - len(text.lstrip())].count("\n")
        line += text.count("\n")
        start = i + 1
        if ch == "{":
            stack.append(text.strip())
            continue
        decl = text.strip()
        if ":" in decl and stack:
            in_root = any(s.startswith(":root") for s in stack)
            prop, value = (p.strip() for p in decl.split(":", 1))
            prop = prop.lower()
            if not in_root:
                if COLOUR.search(value) or (COLOUR_PROPS.match(prop) and NAMED.search(value)):
                    problems.append((at, "colour outside :root (use a variable): %s" % decl))
                if prop == "font-size" and FONT_SIZE.search(value):
                    problems.append((at, "font size outside the scale (use var(--fs-...)): %s" % decl))
                if prop == "font" and FONT_SIZE.search(value.split("/")[0]):
                    problems.append((at, "font size outside the scale (use var(--fs-...)): %s" % decl))
        if ch == "}" and stack:
            stack.pop()
    return problems


def main():
    files = sys.argv[1:] or sorted(glob.glob(os.path.join(ROOT, "web", "css", "*.css")))
    bad = 0
    for f in files:
        for line, msg in check(f):
            print("%s:%d: %s" % (os.path.relpath(f, ROOT), line, msg))
            bad += 1
    if bad:
        print("%d rule(s) outside the design scale in the :root block of web/css/style.css (#317)" % bad)
        sys.exit(1)
    print("web/css: colours and font sizes keep to the design scale (%d files)" % len(files))


if __name__ == "__main__":
    main()
