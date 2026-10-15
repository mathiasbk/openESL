#!/usr/bin/env python3
"""
Build script for the tag firmware. Works with and without make (the Makefile only calls this script).

  python tools/build.py test      host tests (a C compiler on the PC)
  python tools/build.py preview   draws the example layouts to build/preview/*.png
  python tools/build.py fw        firmware with SDCC -> build/fw/openesl.ihx and .bin
                                  (MODEL=154 for the 1.54" -> build/fw154/openesl.bin; default 29)
  python tools/build.py hwtest    HAL bring-up program -> build/hwtest/hwtest.bin
  python tools/build.py fonts     regenerates fonts/font_*.c from fonts/src/
  python tools/build.py clean

Host compiler: the CC environment variable, otherwise gcc, clang, cc, and finally
"python -m ziglang cc" if the ziglang package is installed.
"""
import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TAG = os.path.dirname(HERE)
ROOT = os.path.dirname(TAG)
BUILD = os.path.join(TAG, "build")

SDCC_VERSION = "4.5.0"          # locked version (requirement G5). Another version gives different code.
CODE_SIZE = 0xF400              # 0xF400-0xF7FF: layout file store, 0xF800-0xFBFF: record store (hal_*store_*); 0xFC00 up: the stock MAC

INCLUDES = [os.path.join(TAG, "src"), os.path.join(ROOT, "shared"), os.path.join(TAG, "test")]

CORE = ["src/proto.c", "src/font.c", "src/draw.c", "src/layout.c"]
FONTS = sorted(os.path.relpath(p, TAG) for p in glob.glob(os.path.join(TAG, "fonts", "font_*.c")))

HOST_TARGETS = {
    "test_all": CORE + FONTS + ["src/app.c", "test/test_main.c", "test/test_proto.c",
                                "test/test_font.c", "test/test_draw.c", "test/test_layout.c",
                                "test/test_app.c", "test/fake_hal.c", "test/fake_epd.c",
                                "test/helpers.c"],
    "test_epd": CORE + FONTS + ["src/epd.c", "test/test_epd.c", "test/fake_hal.c", "test/helpers.c"],
    "preview": CORE + FONTS + ["test/preview.c", "test/fake_epd.c", "test/helpers.c"],
}

HAL_SOURCES = ["src/hal/zbs243/hal_zbs243.c", "src/hal/zbs243/radio_zbs243.c"]
FW_SOURCES = ["src/main.c", "src/app.c", "src/epd.c"] + CORE + FONTS + HAL_SOURCES
HWTEST_SOURCES = ["src/hal/zbs243/hwtest.c"] + HAL_SOURCES


def find_cc():
    if os.environ.get("CC"):
        return os.environ["CC"].split()
    for c in ("gcc", "clang", "cc"):
        if shutil.which(c):
            return [c]
    try:
        import ziglang  # noqa: F401
        # Zig's cache fails inside OneDrive folders; put it in build/
        cache = os.path.join(BUILD, "zigcache")
        os.environ.setdefault("ZIG_GLOBAL_CACHE_DIR", cache)
        os.environ.setdefault("ZIG_LOCAL_CACHE_DIR", cache)
        return [sys.executable, "-m", "ziglang", "cc"]
    except ImportError:
        pass
    sys.exit("No C compiler found for the host tests. Set CC, or install gcc/clang "
             "(or 'pip install ziglang').")


def run(cmd, **kw):
    print(" ".join(cmd) if len(cmd) < 8 else " ".join(cmd[:6]) + " ...")
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        sys.exit(r.returncode)


def build_host(name):
    cc = find_cc()
    out_dir = os.path.join(BUILD, "host")
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, name + (".exe" if os.name == "nt" else ""))
    flags = ["-std=c99", "-Wall", "-Wextra", "-O1", "-g"]
    flags += os.environ.get("HOST_CFLAGS", "").split()
    srcs = [os.path.join(TAG, s) for s in HOST_TARGETS[name]]
    run(cc + flags + ["-I" + i for i in INCLUDES] + srcs + ["-o", exe])
    return exe


def cmd_test():
    for name in ("test_all", "test_epd"):
        exe = build_host(name)
        run([exe])
    run([sys.executable, "-m", "unittest", "discover", "-s", os.path.join(TAG, "test"),
         "-p", "test_label_*.py"])


def cmd_preview():
    exe = build_host("preview")
    out = os.path.join(BUILD, "preview")
    os.makedirs(out, exist_ok=True)
    run([exe, out])
    try:
        from PIL import Image
    except ImportError:
        print("Pillow is missing: the PPM files are in", out)
        return
    for ppm in glob.glob(os.path.join(out, "*.ppm")):
        img = Image.open(ppm)
        img.resize((img.width * 2, img.height * 2), Image.NEAREST).save(ppm[:-4] + ".png")
        os.remove(ppm)
    print("Images in", out)


def ihx_to_bin(ihx, bin_path):
    mem = {}
    with open(ihx) as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":"):
                continue
            data = bytes.fromhex(line[1:])
            n, addr, typ = data[0], (data[1] << 8) | data[2], data[3]
            if typ == 0:
                for i in range(n):
                    mem[addr + i] = data[4 + i]
            elif typ == 1:
                break
    if not mem:
        sys.exit("empty ihx")
    top = max(mem) + 1
    if top > CODE_SIZE:
        sys.exit("the firmware goes past 0x%04X and would overwrite the MAC area" % CODE_SIZE)
    img = bytearray([0xFF] * top)
    for a, v in mem.items():
        img[a] = v
    with open(bin_path, "wb") as f:
        f.write(img)
    print("%s: %d bytes (%.1f %% of 0x%04X)" % (bin_path, top, 100.0 * top / CODE_SIZE, CODE_SIZE))


def build_8051(subdir, name, sources, defines=()):
    sdcc = os.environ.get("SDCC", "sdcc")
    winget_sdcc = r"C:\Program Files\SDCC\bin\sdcc.exe"   # winget installs SDCC here without adding it to PATH
    if not shutil.which(sdcc) and os.path.exists(winget_sdcc):
        sdcc = winget_sdcc
        # sdcpp finds cc1 only through PATH
        os.environ["PATH"] = os.path.dirname(winget_sdcc) + os.pathsep + os.environ["PATH"]
    if not shutil.which(sdcc):
        sys.exit("sdcc not found. Install SDCC %s." % SDCC_VERSION)
    ver = subprocess.run([sdcc, "--version"], capture_output=True, text=True).stdout
    if SDCC_VERSION not in ver:
        print("WARNING: expected SDCC %s, got: %s" % (SDCC_VERSION, ver.strip().splitlines()[0]))
    out = os.path.join(BUILD, subdir)
    os.makedirs(out, exist_ok=True)
    flags = ["-mmcs51", "--model-large", "--stack-auto", "--std-c99", "--opt-code-size"]
    # ZBS243 XRAM is 8 KB at 0xE000-0xFFFF, but 0xE000-0xE2FF is lost in deep sleep,
    # so variables start at 0xE300 (kildelogg 2026-10-14; OEPL links there too)
    link_flags = ["--xram-loc", "0xE300", "--xram-size", "0x1D00"]
    rels = []
    for s in sources:
        rel = os.path.join(out, os.path.splitext(os.path.basename(s))[0] + ".rel")
        run([sdcc] + flags + list(defines) + ["-I" + i for i in INCLUDES[:2]] +
            ["-c", os.path.join(TAG, s), "-o", rel])
        rels.append(rel)
    ihx = os.path.join(out, name + ".ihx")
    run([sdcc] + flags + link_flags + ["--code-size", str(CODE_SIZE), "-o", ihx] + rels)
    ihx_to_bin(ihx, os.path.join(out, name + ".bin"))


def cmd_fw():
    model = os.environ.get("MODEL", "29")
    if model not in ("29", "154"):
        sys.exit("MODEL must be 29 or 154")
    build_8051("fw" if model == "29" else "fw" + model, "openesl", FW_SOURCES, ["-DMODEL=" + model])


def cmd_hwtest():
    build_8051("hwtest", "hwtest", HWTEST_SOURCES)


def cmd_fonts():
    run([sys.executable, os.path.join(HERE, "gen_font.py")])


def cmd_clean():
    shutil.rmtree(BUILD, ignore_errors=True)


def main():
    cmds = {"test": cmd_test, "preview": cmd_preview, "fw": cmd_fw, "hwtest": cmd_hwtest, "fonts": cmd_fonts, "clean": cmd_clean}
    if len(sys.argv) != 2 or sys.argv[1] not in cmds:
        sys.exit(__doc__)
    cmds[sys.argv[1]]()


if __name__ == "__main__":
    main()
