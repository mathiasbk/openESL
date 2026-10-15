"""
apidocs.py - the API tab of the web page, made from docs/protokoll.md at build time.

Parts 2 (the HTTP API) and 3 (layout files) of protokoll.md are turned into HTML and put
into the API tab of index.html, so the page and the repository always say the same.
Only the Markdown that protokoll.md uses is understood: headings, paragraphs, bullet
lists (one level of nesting, and indented paragraphs inside an item), fenced code,
tables, `code`, **bold** and [links](url). Relative links point to the repository.
"""
import html
import re

REPO = "https://github.com/mathiasbk/openesl/blob/master/docs/"
PARTS = ("## 2.", "## 3.")


def _inline(text):
    out, pos = [], 0
    for m in re.finditer(r"`([^`]+)`", text):
        out.append(_plain(text[pos:m.start()]))
        out.append("<code>%s</code>" % html.escape(m.group(1), quote=False))
        pos = m.end()
    out.append(_plain(text[pos:]))
    return "".join(out)


def _plain(text):
    text = html.escape(text, quote=False)
    text = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", text)

    def link(m):
        url = m.group(2)
        if not re.match(r"[a-z]+:|#", url):
            url = REPO + url
        return '<a href="%s">%s</a>' % (html.escape(url), m.group(1))
    return re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", link, text)


def _slug(text):
    return "api-" + re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


def _cells(line):
    return [c.strip() for c in line.strip().strip("|").split("|")]


def md_to_html(md):
    lines = md.split("\n")
    out = []
    para = []
    lists = []          # open list depths (0 or 2 spaces)
    i = 0

    def flush_para():
        if para:
            out.append("<p>%s</p>" % _inline(" ".join(para)))
            para.clear()

    def close_lists(depth=-1):
        while lists and lists[-1] > depth:
            lists.pop()
            out.append("</li></ul>")

    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        if stripped.startswith("```"):
            flush_para()
            code = []
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("```"):
                code.append(lines[i])
                i += 1
            out.append("<pre><code>%s</code></pre>" % html.escape("\n".join(code), quote=False))
        elif stripped.startswith("|") and i + 1 < len(lines) and re.match(r"^\|[-|\s:]+\|$", lines[i + 1].strip()):
            flush_para()
            close_lists()
            head = _cells(line)
            out.append('<div class="table"><table><thead><tr>%s</tr></thead><tbody>' %
                       "".join("<th>%s</th>" % _inline(c) for c in head))
            i += 2
            while i < len(lines) and lines[i].strip().startswith("|"):
                out.append("<tr>%s</tr>" % "".join("<td>%s</td>" % _inline(c) for c in _cells(lines[i])))
                i += 1
            out.append("</tbody></table></div>")
            continue
        elif re.match(r"^#{2,4} ", line):
            flush_para()
            close_lists()
            level = len(line) - len(line.lstrip("#"))
            text = line[level:].strip()
            out.append('<h%d id="%s">%s</h%d>' % (level, _slug(text), _inline(text), level))
        elif re.match(r"^ {0,2}- ", line):
            flush_para()
            depth = len(line) - len(line.lstrip(" "))
            if lists and lists[-1] == depth:
                out.append("</li><li>")
            elif lists and lists[-1] > depth:
                close_lists(depth)
                out.append("</li><li>")
            else:
                out.append("<ul><li>")
                lists.append(depth)
            para.append(stripped[2:])
        elif not stripped:
            flush_para()
            if not (i + 1 < len(lines) and lines[i + 1].startswith("  ") and lists):
                close_lists()
        else:
            indent = len(line) - len(line.lstrip(" "))
            if lists and indent and not para:
                close_lists(indent - 2)     # a paragraph inside the item one level up
            para.append(stripped)
        i += 1
    flush_para()
    close_lists()
    return "\n".join(out)


def api_section(protokoll_md):
    """Parts 2 and 3 of protokoll.md as HTML."""
    start = protokoll_md.index(PARTS[0])
    return md_to_html(protokoll_md[start:])
