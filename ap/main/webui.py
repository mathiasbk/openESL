#!/usr/bin/env python3
"""Packs tabs of the web page into the AP firmware, served at /dashboard.

    webui.py WEB_DIR OUT.c [--wasm DIR] [MODULE ...]          (default: dashboard)
    webui.py --renderer-deps DIR      the files a packed renderer in DIR depends on, one per
                                      line, for ap/main/CMakeLists.txt

One page, one source: each module is a tab of WEB_DIR/index.html (MODULES below), and the
board's Kconfig picks which ones its firmware has room for (ESL_WEB_DASHBOARD,
ESL_WEB_DESIGNER), so the C6 gets the Dashboard and a board with flash and PSRAM to spare
also gets the Design tab. The page shows the chosen tabs (js/aptabs.js) and links to the web
page for the rest; each module's js/ap*.js starts it against the AP's own /api/. The Design
tab also gets the renderer (render-<build>.js/.wasm per tag model, built by
`python web/build.py --wasm` into web/dist/wasm/, or the DIR given), so its preview is the
same code as the firmware and works without the internet. Every file
is gzipped (fixed mtime, so the build is reproducible) into a C table that http_api.c serves
with Content-Encoding: gzip from flash. Run by ap/main/CMakeLists.txt.
"""
import gzip
import importlib.util
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Per module: its tab in index.html, the script that starts it on the AP, and every module
# that script imports (checked below, so a new import cannot break the page on the AP), the
# stylesheets past style.css, and whether it needs the example layout files
MODULES = {
    "dashboard": {
        "label": "Dashboard",
        "js": ["apdash.js", "dashboard.js", "apclient.js", "apserial.js", "apport.js", "zbs.js", "dashdata.js",
               "fwupload.js", "sendtext.js", "models.js", "apsettings.js", "backup.js"],
        "css": [],
    },
    "design": {
        "label": "Design",
        "js": ["apdesign.js", "editor.js", "history.js", "limits.js", "snap.js", "apclient.js", "renderer.js",
               "models.js", "tplpicker.js", "catalog.js"],
        "css": ["designer.css"],
        "examples": True,
        "wasm": True,
    },
}
PAGE = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>OpenESL AP dashboard</title>
  <link rel="icon" href="https://mathiasbk.github.io/openESL/img/favicon.svg" type="image/svg+xml">
  <link rel="stylesheet" href="/ui/css/style.css">{css}
</head>
<body>
  <header class="top">
    <h1>OpenESL</h1>
    <nav class="tabs">{tabs}
      <a href="/">Status</a>
      <a href="/setup">WiFi and token</a>
      <a href="https://mathiasbk.github.io/openESL/">{more}</a>
    </nav>
  </header>
{main}
  <script type="module" src="/ui/js/aptabs.js"></script>
{scripts}
</body>
</html>
"""


def tab_html(index, name):
    m = re.search(r'\n(\s*<main id="tab-%s".*?</main>)' % name, index, re.S)
    if not m:
        sys.exit(f"webui.py: no <main id=\"tab-{name}\"> in index.html")
    # a plain <main> here, not a tab panel; js/aptabs.js shows the one picked
    main = m.group(1).replace(' role="tabpanel"', "", 1)
    if name == "dashboard":
        # the text about USB and the bridge is for the page on the web
        main = re.sub(r"\n\s*<p>The AP's state and every tag.*?</p>",
                      "\n      <p>The AP's state and every tag, read from its API and refreshed every 10 seconds. "
                      "Enter the API token once; this browser keeps it for this address.</p>",
                      main, count=1, flags=re.S)
    return main


def page_html(index, modules):
    mains = "\n".join(tab_html(index, name) for name in modules)
    # one tab needs no tab buttons; with more, each is a link to #name (js/aptabs.js)
    tabs = "".join(f'\n      <a href="#{name}" data-tab="{name}">{MODULES[name]["label"]}</a>'
                   for name in modules) if len(modules) > 1 else ""
    css = "".join(f'\n  <link rel="stylesheet" href="/ui/css/{c}">' for name in modules for c in MODULES[name]["css"])
    scripts = "".join(f'  <script type="module" src="/ui/js/{MODULES[name]["js"][0]}"></script>\n' for name in modules)
    more = "Flashers" if "design" in modules else "Designer and flashers"
    return PAGE.format(main=mains, tabs=tabs, css=css, scripts=scripts.rstrip("\n"), more=more)


def example_layouts():
    """The example layout file of each tag model (tag/designs/layouts/<model>.json), which the
    Design tab offers as templates, as web/build.py copies them for the web page"""
    sys.path.insert(0, os.path.join(ROOT, "shared"))
    import models
    for m in models.load():
        yield m["name"], os.path.join(ROOT, "tag", "designs", "layouts", m["name"] + ".json")


def web_build():
    """web/build.py, which knows what the renderer is built from (renderer_inputs)"""
    spec = importlib.util.spec_from_file_location("web_build", os.path.join(ROOT, "web", "build.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def renderer_paths(wasm):
    """[(served path, MIME type, file)] of the renderer for each tag model"""
    sys.path.insert(0, os.path.join(ROOT, "shared"))
    import models
    return [(f"/ui/wasm/render-{m['build']}{ext}", mime, os.path.join(wasm, "render-" + m["build"] + ext))
            for m in models.load() for ext, mime in ((".js", "text/javascript"), (".wasm", "application/wasm"))]


def renderer_files(wasm):
    """[(path, MIME type, bytes)] of the renderer for each tag model, from the directory
    web/build.py --wasm wrote; it must be built from the sources as they are now"""
    newest, src = 0, None
    for f in web_build().renderer_inputs():
        t = os.path.getmtime(f)
        if t > newest:
            newest, src = t, os.path.relpath(f, ROOT)
    files = []
    for served, mime, path in renderer_paths(wasm):
        if not os.path.isfile(path):
            sys.exit(f"webui.py: no {path}: the Design tab needs the renderer, "
                     "build it with Emscripten first: python web/build.py --wasm")
        if os.path.getmtime(path) < newest:
            sys.exit(f"webui.py: {path} is older than {src}: build it again with python web/build.py --wasm")
        with open(path, "rb") as f:
            files.append((served, mime, f.read()))
    return files


def c_bytes(data):
    return ",".join(str(b) for b in data)


def page_files(web, modules, wasm=None):
    """[(path, MIME type, bytes)] the AP serves for these modules (wasm: where the renderer is,
    default WEB_DIR/dist/wasm)"""
    for name in modules:
        if name not in MODULES:
            sys.exit(f"webui.py: no module {name} (there are {', '.join(MODULES)})")
    with open(os.path.join(web, "index.html"), encoding="utf-8") as f:
        files = [("/dashboard", "text/html; charset=utf-8", page_html(f.read(), modules).encode())]
    for css in ["style.css"] + [c for name in modules for c in MODULES[name]["css"]]:
        with open(os.path.join(web, "css", css), "rb") as f:
            files.append((f"/ui/css/{css}", "text/css", f.read()))
    js = ["aptabs.js"] + list(dict.fromkeys(j for name in modules for j in MODULES[name]["js"]))
    for name in js:
        with open(os.path.join(web, "js", name), "rb") as f:
            src = f.read()
        # every module the page imports must be packed too, or the page breaks on the AP
        for dep in re.findall(rb'from "\./([\w.-]+\.js)"', src):
            if dep.decode() not in js:
                sys.exit(f"webui.py: js/{name} imports {dep.decode()}, which is not packed")
        files.append((f"/ui/js/{name}", "text/javascript", src))
    if any(MODULES[name].get("examples") for name in modules):
        for model, path in example_layouts():
            with open(path, "rb") as f:
                files.append((f"/ui/designs/layouts/{model}.json", "application/json", f.read()))
    if any(MODULES[name].get("wasm") for name in modules):
        files += renderer_files(wasm or os.path.join(web, "dist", "wasm"))
    return files


def main():
    if sys.argv[1:2] == ["--renderer-deps"]:
        # the renderer files themselves and what they are built from, so that a change to
        # either runs this script again and its check sees a stale renderer (only the renderer
        # files there now: a missing one would stop ninja before this script can say why)
        built = [p for _, _, p in renderer_paths(sys.argv[2]) if os.path.isfile(p)]
        print("\n".join(built + web_build().renderer_inputs()))
        return
    web, out, args = sys.argv[1], sys.argv[2], sys.argv[3:]
    wasm = None
    if args[:1] == ["--wasm"]:
        wasm, args = args[1], args[2:]
    files = page_files(web, args or ["dashboard"], wasm)
    lines = ["/* Generated by webui.py from web/ - do not edit. */", '#include "webui.h"', ""]
    entries = []
    for i, (path, mime, data) in enumerate(files):
        gz = gzip.compress(data, compresslevel=9, mtime=0)
        lines.append(f"static const unsigned char f{i}[{len(gz)}] = {{{c_bytes(gz)}}};")
        entries.append(f'    {{"{path}", "{mime}", f{i}, {len(gz)}}},')
    lines += ["", "const webui_file_t webui_files[] = {"] + entries + ["    {0},", "};", ""]
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w") as f:
        f.write("\n".join(lines))
    total = sum(len(gzip.compress(d, 9, mtime=0)) for _, _, d in files)
    print(f"webui.py: {len(files)} files, {total} bytes gzipped")


if __name__ == "__main__":
    main()
