#!/usr/bin/env python3
"""Writes the golden file of a release for the compatibility tests (README.md here).

  python3 ap/test/compat/make_golden.py v0.10.0

Takes the AP core of that release from git (git archive, nothing in the working tree
changes), builds gen_golden.c from today's tree against it, and runs it: the result is
ap/test/compat/<release>.txt, which test_compat.c reads. Run it once per release, in the
VERSION PR, and commit the file; never write one for a release again, since the point
is that it holds what that release wrote.
"""
import glob
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))


def main(argv):
    if len(argv) != 2 or not argv[1].startswith("v"):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    release = argv[1]
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory() as tmp:
        tar = os.path.join(tmp, "src.tar")
        subprocess.run(["git", "-C", ROOT, "archive", "-o", tar, release, "ap/components", "ap/test/layouts",
                        "shared", "tag/src", "tag/fonts"], check=True)
        src = os.path.join(tmp, "src")
        with tarfile.open(tar) as t:
            t.extractall(src, filter="data")
        core = os.path.join(src, "ap", "components", "esl_core")
        coproc = os.path.join(src, "ap", "components", "esl_coproc")

        def has(header, text):
            with open(os.path.join(core, "include", header), encoding="utf-8") as f:
                return text in f.read()

        flags = []
        if has("esl_radio.h", "no_link"):
            flags.append("-DHAS_NO_LINK")
        if has("esl_proto.h", "long_poll_s"):
            flags.append("-DHAS_LONG")
        srcs = sorted(glob.glob(os.path.join(core, "*.c")) + glob.glob(os.path.join(coproc, "*.c")) +
                      glob.glob(os.path.join(src, "tag", "fonts", "font_*.c")))
        srcs += [os.path.join(src, "tag", "src", f) for f in ("font.c", "barcode.c")]
        gen = os.path.join(tmp, "gen_golden")
        subprocess.run([cc, "-std=c11", "-O1", "-w", *flags,
                        "-I" + os.path.join(core, "include"), "-I" + os.path.join(coproc, "include"),
                        "-I" + os.path.join(src, "shared"), "-I" + os.path.join(src, "tag", "src"),
                        os.path.join(HERE, "gen_golden.c"), *srcs, "-o", gen], check=True)
        out = os.path.join(HERE, release + ".txt")
        subprocess.run([gen, release, os.path.join(src, "ap", "test", "layouts", "2.9.json"), out], check=True)
    print(f"{os.path.relpath(out, ROOT)} written ({' '.join(flags) or 'no flags'})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
