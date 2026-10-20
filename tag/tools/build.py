#!/usr/bin/env python3
"""
Build script for the tag firmware. Works with and without make (the Makefile only calls this script).

  python tools/build.py test      host tests (a C compiler on the PC)
  python tools/build.py preview   draws the example layouts to build/preview/*.png
  python tools/build.py fw        firmware with SDCC -> build/fw/openesl.bin (bootloader + application,
                                  for the flasher) and build/fw/openesl-ota.bin (the application
                                  with the OTA header, for the AP; docs/tag-ota.md)
                                  (MODEL=<build> from shared/models.json: 154 for the 1.54"
                                  -> build/fw154/; default 29)
  python tools/build.py hwtest    HAL bring-up program -> build/hwtest/hwtest.bin
                                  (HWTEST_MAIN=src/hal/zbs243/adcprobe.c builds an experiment instead)
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

sys.path.insert(0, os.path.join(ROOT, "shared"))
import models  # noqa: E402  (the model registry, shared/models.json)

SDCC_VERSION = "4.5.0"          # locked version (requirement G5). Another version gives different code.
CODE_SIZE = 0xF400              # 0xF400-0xF7FF: layout file store, 0xF800-0xFBFF: record store (hal_*store_*); 0xFC00 up: the stock MAC
BOOT_SIZE = 0x400               # page 0: the bootloader (boot/); the application starts here (ESL_FW_APP_BASE)

INCLUDES = [os.path.join(TAG, "src"), os.path.join(ROOT, "shared"), os.path.join(TAG, "test"),
            os.path.join(TAG, "boot")]

OTA_FILE_MAGIC = 0x5746534F     # ESL_FWF_MAGIC in shared/proto.h
OTA_FILE_FORMAT = 1

CORE = ["src/proto.c", "src/font.c", "src/draw.c", "src/layout.c", "src/barcode.c"]
FONTS = sorted(os.path.relpath(p, TAG) for p in glob.glob(os.path.join(TAG, "fonts", "font_*.c")))

HOST_TARGETS = {
    "test_all": CORE + FONTS + ["src/app.c", "src/ota.c", "boot/boot_core.c", "test/fake_xflash.c",
                                "test/test_ota.c", "test/test_main.c", "test/test_proto.c",
                                "test/test_font.c", "test/test_draw.c", "test/test_layout.c",
                                "test/test_app.c", "test/test_barcode.c", "test/fake_hal.c", "test/fake_epd.c",
                                "test/helpers.c"],
    "test_epd": CORE + FONTS + ["src/epd.c", "test/test_epd.c", "test/fake_hal.c", "test/helpers.c"],
    "preview": CORE + FONTS + ["test/preview.c", "test/fake_epd.c", "test/helpers.c"],
}



def read_version(model):
    """The firmware version of one tag model (build "29", "154"), as ("1.2.3", 10203): the release
    the model's sources last changed in (tools/fw_version.py), so a release that changes only the
    AP, or only another model, gives these tags nothing new.
    The number is what the tag sends in POLL (u16), so major is at most 6 and minor and patch at most 99."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import fw_version
    text = fw_version.version("tag-" + model)
    parts = text.split(".")
    if len(parts) != 3 or not all(p.isdigit() for p in parts):
        sys.exit("VERSION must be major.minor.patch, got %r" % text)
    major, minor, patch = (int(p) for p in parts)
    if minor > 99 or patch > 99 or major * 10000 + minor * 100 + patch > 0xFFFF:
        sys.exit("VERSION %s does not fit in the u16 the tag sends" % text)
    return text, major * 10000 + minor * 100 + patch


def version_defines(model):
    text, number = read_version(model)
    return ["-DFW_VERSION=%d" % number, '-DFW_VERSION_STR="%s"' % text]


def host_model():
    """The model the host tests are built for: -DMODEL=<build> in HOST_CFLAGS, else the first"""
    for flag in os.environ.get("HOST_CFLAGS", "").split():
        if flag.startswith("-DMODEL="):
            return flag[len("-DMODEL="):]
    return models.load()[0]["build"]


HAL_SOURCES = ["src/hal/zbs243/hal_zbs243.c", "src/hal/zbs243/radio_zbs243.c"]
FW_SOURCES = ["src/main.c", "src/app.c", "src/epd.c", "src/ota.c"] + CORE + FONTS + HAL_SOURCES + \
    ["src/hal/zbs243/xflash_zbs243.c"]
BOOT_SOURCES = ["boot/boot_zbs243.c", "boot/boot_core.c"]
HWTEST_SOURCES = [os.environ.get("HWTEST_MAIN", "src/hal/zbs243/hwtest.c")] + HAL_SOURCES


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
    run(cc + flags + version_defines(host_model()) + ["-I" + i for i in INCLUDES] + srcs + ["-o", exe])
    return exe


def cmd_test():
    # test_barcode writes its screens here for test_label_barcode.py to decode
    os.environ["ESL_TEST_OUT"] = os.path.join(BUILD, "host", "barcodes")
    os.makedirs(os.environ["ESL_TEST_OUT"], exist_ok=True)
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


def ihx_to_bin(ihx, bin_path, base=0, limit=CODE_SIZE):
    """The image from base up to the last byte the ihx sets; must end below limit."""
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
    if min(mem) < base:
        sys.exit("%s has code below 0x%04X" % (ihx, base))
    top = max(mem) + 1
    if top > limit:
        sys.exit("%s goes past 0x%04X" % (ihx, limit))
    img = bytearray([0xFF] * (top - base))
    for a, v in mem.items():
        img[a - base] = v
    with open(bin_path, "wb") as f:
        f.write(img)
    print("%s: %d bytes (%.1f %% of 0x%04X)" % (bin_path, len(img), 100.0 * len(img) / (limit - base), limit - base))
    return bytes(img)


def crc32(data):
    import zlib
    return zlib.crc32(data) & 0xFFFFFFFF


def build_8051(subdir, name, sources, defines=(), code_loc=0, limit=CODE_SIZE, model="--model-large",
               tag_model=None):
    """Compiles and links; returns the image from code_loc as bytes (also written to <name>.bin)."""
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
    flags = ["-mmcs51", model, "--std-c99", "--opt-code-size"]
    if model == "--model-large":
        flags.append("--stack-auto")
    # ZBS243 XRAM is 8 KB at 0xE000-0xFFFF, but 0xE000-0xE2FF is lost in deep sleep,
    # so variables start at 0xE300 (kildelogg 2026-10-14; OEPL links there too)
    link_flags = ["--xram-loc", "0xE300", "--xram-size", "0x1D00",
                  "--code-loc", str(code_loc), "--code-size", str(limit - code_loc)]
    rels = []
    for s in sources:
        rel = os.path.join(out, os.path.splitext(os.path.basename(s))[0] + ".rel")
        run([sdcc] + flags + list(defines) + version_defines(tag_model or models.load()[0]["build"]) +
            ["-I" + i for i in INCLUDES[:2]] +
            ["-c", os.path.join(TAG, s), "-o", rel])
        rels.append(rel)
    ihx = os.path.join(out, name + ".ihx")
    run([sdcc] + flags + link_flags + ["-o", ihx] + rels)
    return ihx_to_bin(ihx, os.path.join(out, name + ".bin"), code_loc, limit)


def cmd_fw():
    """Bootloader in page 0 and the application from BOOT_SIZE (docs/tag-ota.md):
         openesl.bin      both, written at 0 by the flasher (the jig)
         openesl-ota.bin  the OTA file for the AP: ESL_FWF_* header + the application"""
    import struct
    ms = models.load()
    m = models.by_build(os.environ.get("MODEL", ms[0]["build"]), ms)
    if not m:
        sys.exit("MODEL must be one of %s (shared/models.json)" % ", ".join(x["build"] for x in ms))
    model = m["build"]
    subdir = models.fw_dir(m)
    boot = build_8051(subdir + "/boot", "boot", BOOT_SOURCES, code_loc=0, limit=BOOT_SIZE, model="--model-small")
    app = build_8051(subdir, "app", FW_SOURCES, ["-DMODEL=" + model], code_loc=BOOT_SIZE, limit=CODE_SIZE,
                     tag_model=model)
    app += b"\xFF" * (-len(app) % 1024)   # the bootloader copies whole 1 KB pages
    out = os.path.join(BUILD, subdir)
    with open(os.path.join(out, "openesl.bin"), "wb") as f:
        f.write(boot + b"\xFF" * (BOOT_SIZE - len(boot)) + app)
    code = m["code"]                        # ESL_MODEL_* in shared/models_gen.h
    _, fw_ver = read_version(model)
    hdr = struct.pack("<IBBHII", OTA_FILE_MAGIC, OTA_FILE_FORMAT, code, fw_ver, len(app), crc32(app))
    with open(os.path.join(out, "openesl-ota.bin"), "wb") as f:
        f.write(hdr + app)
    print("%s: bootloader + application; openesl-ota.bin: %d bytes for the AP" %
          (os.path.join(out, "openesl.bin"), len(hdr) + len(app)))


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
