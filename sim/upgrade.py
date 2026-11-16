#!/usr/bin/env python3
"""Tag firmware over the air from a release to today's firmware, and back, in the simulator
(#307, README.md "Upgrade from a release").

  python3 sim/upgrade.py [v0.10.0 ...]     (make -C sim upgrade; SAN=1 for the sanitizers)

For each release (by default every one with a golden file in ap/test/compat/, which the
VERSION PR adds) and each tag model both have, it builds esl-sim-tag twice: from that
release's tag firmware (tag/ and shared/ from git archive, nothing in the working tree
changes) and from today's, both with today's sim_tag.c, tagstep.c and png.c. Then
"esl-sim upgrade" (upgrade.c) runs today's AP core with the release's tag, sends it today's
firmware, and lets today's tag go on from the flash the release's bootloader left; and the
same the other way round, back to the release.

Each firmware reports the version tools/fw_version.py gives it. When a model's firmware
has not changed since the release, the two versions are the same and a tag would not take
the update, so today's is tested as the next patch version.
"""
import glob
import json
import os
import re
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import fw_version  # noqa: E402

TAG_SRC = ["src/app.c", "src/proto.c", "src/font.c", "src/draw.c", "src/status.c", "src/barcode.c", "src/ota.c",
           "src/image.c", "boot/boot_core.c", "test/fake_epd.c", "test/fake_epd_image.c", "test/fake_xflash.c"]
SIM_SRC = ["sim_tag.c", "tagstep.c", "png.c"]


def code(v):
    major, minor, patch = (int(n) for n in v.split("."))
    return major * 10000 + minor * 100 + patch


def builds(root):
    """The tag models (build names) of shared/models.json under root"""
    with open(os.path.join(root, "shared", "models.json"), encoding="utf-8") as f:
        data = json.load(f)
    return [m["build"] for m in (data["models"] if isinstance(data, dict) else data)]


def store_format(root):
    """STORE_FORMAT of tag/src/app.c under root: the format of the settings record"""
    with open(os.path.join(root, "tag", "src", "app.c"), encoding="utf-8") as f:
        m = re.search(r"#define\s+STORE_FORMAT\s+(\d+)", f.read())
    return m and int(m.group(1))


def build_tag(root, model, version, out, san):
    """esl-sim-tag from the tag firmware under root (tag/, shared/), as model, reporting version"""
    tag = os.path.join(root, "tag")
    with open(os.path.join(tag, "src", "image.h"), encoding="utf-8") as f:
        has_mark = re.search(r"\bmark\b", f.read()) is not None
    flags = ["-std=c99", "-g", "-O1", f"-DMODEL={model}", f"-DFW_VERSION={code(version)}",
             f'-DFW_VERSION_STR="{version}"', "-I" + os.path.join(tag, "src"), "-I" + os.path.join(tag, "boot"),
             "-I" + os.path.join(root, "shared"), "-I" + os.path.join(tag, "test"),
             "-w"]   # the warnings of today's code show in the simulator's own build
    if not has_mark:
        flags.append("-DSIM_TAG_NO_MARK")
    with open(os.path.join(tag, "src", "app.h"), encoding="utf-8") as f:
        if re.search(r"\bap_id\b", f.read()) is None:
            flags.append("-DSIM_TAG_NO_APID")
    if san:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    srcs = [os.path.join(tag, s) for s in TAG_SRC] + sorted(glob.glob(os.path.join(tag, "fonts", "font_*.c")))
    srcs += [os.path.join(HERE, s) for s in SIM_SRC]
    os.makedirs(os.path.dirname(out), exist_ok=True)
    subprocess.run([os.environ.get("CC", "cc"), *flags, *srcs, "-o", out], check=True)


def run(sim, a, b, b_version, out, same_store):
    os.makedirs(out, exist_ok=True)
    r = subprocess.run([sim, "upgrade", a, b, str(code(b_version)), out] + (["same-store"] if same_store else []))
    return r.returncode == 0


def main(argv):
    san = os.environ.get("SAN") == "1"
    releases = argv[1:] or sorted((os.path.basename(p)[:-4] for p in glob.glob(
        os.path.join(ROOT, "ap", "test", "compat", "v*.txt"))), key=lambda r: [int(n) for n in r[1:].split(".")])
    if not releases or any(not r.startswith("v") for r in releases):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    today = builds(ROOT)
    failed = []
    with tempfile.TemporaryDirectory() as tmp:
        for release in releases:
            src = os.path.join(tmp, release)
            tar = os.path.join(tmp, release + ".tar")
            if subprocess.run(["git", "-C", ROOT, "archive", "-o", tar, release, "tag", "shared"]).returncode:
                print(f"upgrade.py: no {release} here (git fetch --tags)", file=sys.stderr)
                return 1
            with tarfile.open(tar) as t:
                t.extractall(src, filter="data")
            for model in [m for m in builds(src) if m in today]:
                old_v = fw_version.version("tag-" + model, at=release)
                new_v = fw_version.version("tag-" + model)
                note = ""
                if code(new_v) == code(old_v):
                    major, minor, patch = (int(n) for n in new_v.split("."))
                    new_v = f"{major}.{minor}.{patch + 1}"
                    note = f" (unchanged since {release}: tested as {new_v})"
                print(f"== tag-{model}: {release} firmware {old_v} <-> today's {new_v}{note}", flush=True)
                bin_dir = os.path.join(HERE, "build", "upgrade", f"{release}-{model}")
                old = os.path.join(bin_dir, "esl-sim-tag-" + release)
                new = os.path.join(bin_dir, "esl-sim-tag-today")
                try:
                    build_tag(src, model, old_v, old, san)
                except subprocess.CalledProcessError:
                    print(f"upgrade.py: esl-sim-tag does not build from {release} (sim_tag.c or tagstep.c uses "
                          "something that release's tag/ lacks: add a SIM_TAG_NO_* switch, as for image.mark)",
                          flush=True)
                    failed.append(f"tag-{model} {release} build")
                    continue
                build_tag(ROOT, model, new_v, new, san)
                sim = os.path.join(HERE, "build", model, "esl-sim")
                if not os.path.exists(sim):
                    print(f"upgrade.py: {sim} is missing (make -C sim first)", file=sys.stderr)
                    return 1
                same = store_format(src) == store_format(ROOT)
                for name, a, b, v in (("up", old, new, new_v), ("back", new, old, old_v)):
                    print(f"-- {name}: {os.path.basename(a)} -> {os.path.basename(b)}", flush=True)
                    if not run(sim, a, b, v, os.path.join(bin_dir, name), same):
                        failed.append(f"tag-{model} {release} {name}")
    if failed:
        print("upgrade.py: FAILED: " + ", ".join(failed))
        return 1
    print(f"upgrade.py: every upgrade and the way back passed ({', '.join(releases)})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
