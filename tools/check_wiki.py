#!/usr/bin/env python3
"""
Checks the wiki pages in wiki/ before .github/workflows/wiki.yml publishes them to the
GitHub wiki: every link to another wiki page, to a file in the repository or to an image
must point to something that exists, and every #anchor to a heading that exists.

  python tools/check_wiki.py

Links into the repository point to master (https://github.com/mathiasbk/openESL/blob/master/...)
and are checked against this checkout. The wiki is published from master, so a file that is
on development but not yet on master is there by the time the wiki links to it.
Exit code: 1 when a link is broken, otherwise 0.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WIKI = os.path.join(ROOT, "wiki")
REPO = "mathiasbk/openESL"
BLOB = re.compile(r"^https://github\.com/%s/(?:blob|tree)/master/([^#?]+)(?:#(.*))?$" % REPO, re.I)
RAW = re.compile(r"^https://raw\.githubusercontent\.com/%s/master/([^#?]+)$" % REPO, re.I)
OTHER_BRANCH = re.compile(r"^https://github\.com/%s/(?:blob|tree)/(?!master/)" % REPO, re.I)
RAW_WIKI = re.compile(r"^https://raw\.githubusercontent\.com/wiki/%s/([^#?]+)$" % REPO, re.I)
PAGE = re.compile(r"^([A-Za-z0-9][A-Za-z0-9.-]*)(?:#(.*))?$")
LINK = re.compile(r"\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)|<img[^>]*\ssrc=\"([^\"]+)\"")
HEADING = re.compile(r"^#{1,6}\s+(.*?)\s*#*\s*$")


def slug(heading):
    """The anchor GitHub gives a heading: lower case, punctuation dropped, spaces to dashes."""
    h = re.sub(r"[`*_]|<[^>]+>", "", heading).strip().lower()
    h = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", h)
    return re.sub(r"[^\w\- ]", "", h).replace(" ", "-")


def anchors(path):
    """The anchors of every heading in a file; a heading seen before gets -1, -2, ... as on GitHub."""
    found, seen, fence = set(), {}, False
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.lstrip().startswith("```"):
                fence = not fence
            m = None if fence else HEADING.match(line)
            if m:
                a = slug(m.group(1))
                n = seen.get(a, 0)
                seen[a] = n + 1
                found.add(a if n == 0 else "%s-%d" % (a, n))
    return found


def main():
    pages, errors = {}, []
    for dirpath, _, files in os.walk(WIKI):
        for name in files:
            if name.endswith(".md"):
                page = name[:-3]
                if page in pages:
                    errors.append("%s: page name used twice (also %s); the wiki needs unique names"
                                  % (os.path.relpath(os.path.join(dirpath, name), ROOT), pages[page]))
                pages[page] = os.path.relpath(os.path.join(dirpath, name), ROOT)
    if "Home" not in pages:
        errors.append("wiki/Home.md is missing")

    for page, rel in sorted(pages.items()):
        text = open(os.path.join(ROOT, rel), encoding="utf-8").read()
        text = re.sub(r"```.*?```", "", text, flags=re.S)
        text = re.sub(r"`[^`\n]*`", "", text)          # inline code: examples, not links
        for m in re.finditer(r"\[\[([^\]]+)\]\]", text):
            errors.append("%s: [[%s]]: use a Markdown link, [text](Page), which this check can follow"
                          % (rel, m.group(1)))
        for m in LINK.finditer(text):
            url = m.group(1) or m.group(2)
            where = "%s: %s" % (rel, url)
            if url.startswith("#"):
                if url[1:] not in anchors(os.path.join(ROOT, rel)):
                    errors.append(where + ": no such heading on this page")
            elif OTHER_BRANCH.match(url):
                errors.append(where + ": link to master, which the wiki describes, not another branch")
            elif BLOB.match(url):
                path, anchor = BLOB.match(url).groups()
                full = os.path.join(ROOT, path.replace("%20", " "))
                if not os.path.exists(full):
                    errors.append(where + ": no such file in the repository")
                elif anchor and full.endswith(".md") and anchor not in anchors(full):
                    errors.append(where + ": no such heading in " + path)
            elif RAW.match(url):
                if not os.path.isfile(os.path.join(ROOT, RAW.match(url).group(1))):
                    errors.append(where + ": no such file in the repository")
            elif RAW_WIKI.match(url):
                if not os.path.isfile(os.path.join(WIKI, RAW_WIKI.match(url).group(1))):
                    errors.append(where + ": no such file in wiki/")
            elif re.match(r"^[a-z]+:", url, re.I):
                continue                                # other sites: not checked
            elif PAGE.match(url):
                target, anchor = PAGE.match(url).groups()
                if target not in pages:
                    errors.append(where + ": no such wiki page")
                elif anchor and anchor not in anchors(os.path.join(ROOT, pages[target])):
                    errors.append(where + ": no such heading on " + target)
            else:
                errors.append(where + ": link the wiki cannot follow (use a page name or a full URL)")

    for e in errors:
        print(e)
    print("%d wiki pages, %s" % (len(pages), "%d broken links" % len(errors) if errors else "all links OK"))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
