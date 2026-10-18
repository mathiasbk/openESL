#!/usr/bin/env python3
"""The version of the AP firmware and of the tag firmware, worked out from git.

    fw_version.py ap|tag        prints the version, such as 0.4.5
    fw_version.py --all         prints "ap 0.4.6" and "tag 0.4.5", one per line

The VERSION file names the release. A part of the firmware keeps the version of the release
it last changed in: going back through the release tags (v1.2.3, newest first), as long as
the part's files are the same in a release as in this checkout, the part has that release's
version. So when only the AP changed since v0.4.5, release 0.4.6 has AP firmware 0.4.6 and
tag firmware 0.4.5 again, the tags see no new firmware and nobody sends it to them over the
radio. Nothing is bumped by hand; a file a part is built from changes its version.

Which files a part is built from is in PARTS below. A file listed by mistake only gives a
part a new version it did not need; a file missing from it would give changed firmware the
old version, so the list is wide. Uncommitted changes count (the files are compared with
the working tree).

Without git, or without release tags (a download of the sources, or a shallow clone), the
version is the VERSION file's, as before; a warning says so. CI checks out the whole
history, and the release job checks that the images carry the versions this script gives.

The AP build and the simulation ask this script when they configure, so after a git fetch
that brought new tags, or a change in the sources, a local build that only recompiles can
keep the earlier version: idf.py reconfigure (or a clean build) picks up the new one. CI
builds from scratch.
"""
import ast
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NOT_DOCS = ":(exclude,glob)**/*.md"


def webui_js():
    """The web page's scripts the AP packs into its own dashboard (JS in ap/main/webui.py)"""
    with open(os.path.join(ROOT, "ap", "main", "webui.py"), encoding="utf-8") as f:
        tree = ast.parse(f.read())
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(getattr(t, "id", None) == "JS" for t in node.targets):
            return ast.literal_eval(node.value)
    sys.exit("fw_version.py: no JS list in ap/main/webui.py")


def parts():
    return {
        # tag/tools/build.py holds the SDCC version and the build flags; tag/fonts/src is only
        # the source of the generated tag/fonts/font_*.c
        "tag": ["shared", "tag/src", "tag/boot", "tag/fonts/*.c", "tag/tools/build.py", NOT_DOCS],
        # ap/dependencies.lock holds the ESP-IDF version; the AP serves the dashboard of the web
        # page (index.html, style.css and the scripts in ap/main/webui.py) and draws its status
        # screen with the tag's small font and headers (ap/components/esl_core/CMakeLists.txt)
        "ap": ["shared", "ap", ":(exclude)ap/test", "tag/src", "tag/fonts/*.c", "web/index.html", "web/css/style.css",
               NOT_DOCS] + ["web/js/" + js for js in webui_js()],
    }


def read_version(at=None):
    if at:
        return git("show", "%s:VERSION" % at).stdout.strip()
    with open(os.path.join(ROOT, "VERSION")) as f:
        return f.read().strip()


def git(*args):
    return subprocess.run(["git", "-C", ROOT] + list(args), capture_output=True, text=True)


def release_tags():
    """The release tags (no pre-releases), newest first"""
    r = git("tag", "--list", "v*")
    if r.returncode:
        return None
    tags = [t for t in r.stdout.split() if re.fullmatch(r"v\d+\.\d+\.\d+", t)]
    return sorted(tags, key=lambda t: tuple(int(n) for n in t[1:].split(".")), reverse=True)


def unchanged_since(tag, paths, at=None):
    """The files are the same in the release tag as in the working tree (untracked ones too),
    or as in the commit at"""
    if at:
        return not git("diff", "--quiet", tag, at, "--", *paths).returncode
    if git("diff", "--quiet", tag, "--", *paths).returncode:
        return False
    return not git("ls-files", "--others", "--exclude-standard", "--", *paths).stdout.strip()


def version(part, paths=None, current=None, at=None):
    """The part's version in the working tree, or in the commit at (a branch, a tag, a sha)"""
    paths = paths or parts()[part]
    current = current or read_version(at)
    try:
        tags = release_tags()
    except OSError:
        tags = None
    if not tags:
        print("fw_version.py: no git or no release tags here, %s firmware gets VERSION (%s)" % (part, current),
              file=sys.stderr)
        return current
    found = current
    for tag in tags:
        # a release newer than VERSION (VERSION not bumped yet) says nothing about this one
        if tuple(int(n) for n in tag[1:].split(".")) > tuple(int(n) for n in current.split(".")):
            continue
        if not unchanged_since(tag, paths, at):
            break
        found = tag[1:]
    return found


def main():
    if sys.argv[1:] == ["--all"]:
        for part in ("ap", "tag"):
            print(part, version(part))
    elif len(sys.argv) == 2 and sys.argv[1] in ("ap", "tag"):
        print(version(sys.argv[1]))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
