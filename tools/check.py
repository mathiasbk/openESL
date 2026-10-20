#!/usr/bin/env python3
"""
Builds and tests what CI builds and tests (.github/workflows/firmware.yml and web.yml), on this
machine, so a change can be checked before it is pushed.

  python tools/check.py               every step
  python tools/check.py tag web       only these steps (names below)
  python tools/check.py --list        the steps and what each needs
  python tools/check.py --docker      every step in the openesl-dev Docker image (tools/dev/Dockerfile),
                                      which has all the toolchains: the easy way on Windows and macOS
  python tools/check.py --strict      a step whose tools are missing fails instead of being skipped

Steps:
  models    the generated model files match shared/models.json (tools/gen_models.py --check)
  tag       tag host tests (tag/tools/build.py test)
  sim       end-to-end simulation, with sanitizers, and the serial port test (sim/)
  tag-fw    tag firmware for every model in shared/models.json with SDCC 4.5.0
  ap-test   AP core host tests with sanitizers (ap/test)
  tools     esl-client and power-budget tests
  openapi   docs/openapi.yaml against the AP code
  ap-fw     AP firmware with ESP-IDF v5.5.5 for every board in ap/boards/ (ap/build.py; idf.py
            on PATH, otherwise the espressif/idf:v5.5.5 Docker image)
  web       web page and renderer (Emscripten 3.1.74) and the web tests (Node)

The firmware ends up in out/ with the names CI gives it (tag-29.bin, openesl-ap.bin, ...).
Toolchains: tools/dev/setup.sh installs SDCC, Emscripten and Node into ~/.cache/openesl on Linux
or WSL, and this script finds them there. Exit code: 1 when a step failed, otherwise 0.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "out")
TOOLS = os.environ.get("OPENESL_TOOLS", os.path.join(os.path.expanduser("~"), ".cache", "openesl"))

# Keep these in step with tools/dev/setup.sh and .github/workflows/
SDCC_VERSION = "4.5.0"
EMSCRIPTEN_VERSION = "3.1.74"
IDF_IMAGE = "espressif/idf:v5.5.5"
DEV_IMAGE = "openesl-dev"
PY = sys.executable or "python3"
sys.path.insert(0, os.path.join(ROOT, "shared"))
import models  # noqa: E402  (the model registry, shared/models.json)
WINDOWS = os.name == "nt"


class Skip(Exception):
    pass


def add_tool_dirs():
    """Puts the toolchains from tools/dev/setup.sh on PATH, unless others are there already."""
    path = os.environ.get("PATH", "").split(os.pathsep)
    em = os.path.join(TOOLS, "emscripten-" + EMSCRIPTEN_VERSION)
    extra = [os.path.join(TOOLS, "sdcc-" + SDCC_VERSION, "bin"), os.path.join(em, "install", "emscripten")]
    extra += [os.path.join(TOOLS, d, "bin") for d in sorted(os.listdir(TOOLS)) if d.startswith("node-")] \
        if os.path.isdir(TOOLS) else []
    os.environ["PATH"] = os.pathsep.join(path + [d for d in extra if os.path.isdir(d) and d not in path])
    if "EM_CONFIG" not in os.environ and os.path.isfile(os.path.join(em, "em_config")):
        os.environ["EM_CONFIG"] = os.path.join(em, "em_config")


def need(tool, hint):
    exe = shutil.which(tool)
    if not exe:
        raise Skip("%s not found (%s)" % (tool, hint))
    return exe


def need_python(module, package):
    if subprocess.run([PY, "-c", "import " + module], capture_output=True).returncode:
        raise Skip("Python package %s missing (pip install %s)" % (package, package))


def need_host_cc():
    if not any(shutil.which(c) for c in (os.environ.get("CC", "gcc"), "gcc", "clang", "cc")):
        raise Skip("no C compiler (gcc or clang)")


def need_san():
    if WINDOWS:
        raise Skip("the sanitizer builds need Linux or macOS (use --docker, or WSL)")
    need("make", "GNU make")
    need_host_cc()


def run(cmd, cwd=ROOT, env=None):
    print("+ " + " ".join(cmd), flush=True)
    full_env = dict(os.environ, **(env or {}))
    code = subprocess.run(cmd, cwd=cwd, env=full_env).returncode
    if code:
        raise RuntimeError("%s exited with %d (output above)" % ("%s ... %s" % (cmd[0], cmd[-1][:40]), code))


def copy_out(src, name):
    os.makedirs(OUT, exist_ok=True)
    shutil.copyfile(os.path.join(ROOT, src), os.path.join(OUT, name))


def step_models():
    run([PY, "tools/gen_models.py", "--check"])


def step_tag():
    need_host_cc()
    run([PY, "tag/tools/build.py", "test"])


def step_sim():
    need_san()
    run(["make", "-C", "sim", "SAN=1"])
    run(["make", "-C", "sim", "serial"])


def step_tag_fw():
    sdcc = need("sdcc", "SDCC %s, tools/dev/setup.sh" % SDCC_VERSION)
    version = subprocess.run([sdcc, "--version"], capture_output=True, text=True).stdout
    if " %s " % SDCC_VERSION not in version:
        raise Skip("SDCC %s needed, found: %s" % (SDCC_VERSION, version.splitlines()[0] if version else "?"))
    for m in models.load():
        run([PY, "tag/tools/build.py", "fw"], env={"MODEL": m["build"]})
        copy_out("tag/build/%s/openesl.bin" % models.fw_dir(m), "tag-%s.bin" % m["build"])
        copy_out("tag/build/%s/openesl-ota.bin" % models.fw_dir(m), "tag-%s-ota.bin" % m["build"])


def step_ap_test():
    need_san()
    need_python("zxingcpp", "zxing-cpp==3.1.1")
    need_python("PIL", "pillow")
    run(["make", "-C", "ap/test", "SAN=1"])


def step_tools():
    run([PY, "-m", "unittest", "test_esl_client"], cwd=os.path.join(ROOT, "tools", "esl-client"))
    run([PY, "-m", "unittest", "test_power_budget"], cwd=os.path.join(ROOT, "tools", "power-budget"))


def step_openapi():
    need("make", "GNU make")
    need_host_cc()
    need_python("yaml", "pyyaml")
    run(["make", "-C", "ap/test", "openapi"])


AP_BUILD = "python3 build.py"     # ap/build.py: every board in ap/boards/


def step_ap_fw():
    os.makedirs(os.path.join(ROOT, "ap", "out"), exist_ok=True)
    if shutil.which("idf.py") and not WINDOWS:
        idf = subprocess.run(["idf.py", "--version"], capture_output=True, text=True).stdout.strip()
        if "v5.5.5" not in idf:
            print("note: CI builds with ESP-IDF v5.5.5, this is %s" % idf)
        run(["bash", "-c", AP_BUILD], cwd=os.path.join(ROOT, "ap"))
    else:
        if not docker_ok():
            raise Skip("neither idf.py (ESP-IDF v5.5.5) nor a running Docker")
        run(docker_args(IDF_IMAGE, "/src/ap") + ["bash", "-c", AP_BUILD])
    for name in sorted(os.listdir(os.path.join(ROOT, "ap", "out"))):
        copy_out(os.path.join("ap", "out", name), name)


def step_web():
    need("emcc", "Emscripten %s, tools/dev/setup.sh or emsdk" % EMSCRIPTEN_VERSION)
    node = need("node", "Node 22")
    need_host_cc()
    run([PY, "web/build.py"])
    run([PY, "web/build.py", "--native"])
    tests = sorted(os.path.join("web", "test", f) for f in os.listdir(os.path.join(ROOT, "web", "test"))
                   if f.endswith(".test.mjs"))
    run([node, "--test"] + tests)
    fw = [os.path.join(OUT, f) for f in ["tag-%s.bin" % m["build"] for m in models.load()] + ["openesl-ap.bin", "openesl-ap-lcd.bin"]]
    if all(os.path.isfile(f) for f in fw):
        run([PY, "web/build.py", "--firmware"] + fw)     # web/dist/ is then the page as CI publishes it


STEPS = {
    "models": (step_models, "Python"),
    "tag": (step_tag, "C compiler"),
    "sim": (step_sim, "make, gcc/clang with sanitizers (not native Windows)"),
    "tag-fw": (step_tag_fw, "SDCC " + SDCC_VERSION),
    "ap-test": (step_ap_test, "make, gcc/clang with sanitizers (not native Windows), pip: zxing-cpp pillow"),
    "tools": (step_tools, "Python"),
    "openapi": (step_openapi, "make, C compiler, pip: pyyaml"),
    "ap-fw": (step_ap_fw, "ESP-IDF v5.5.5 (idf.py) or Docker"),
    "web": (step_web, "Emscripten %s, Node 22" % EMSCRIPTEN_VERSION),
}


def docker_ok():
    return shutil.which("docker") and subprocess.run(["docker", "info"], capture_output=True).returncode == 0


def docker_args(image, workdir):
    """docker run with the repository at /src. Files are written as the calling user (Linux),
    and a proxy with its own CA (as in hosted cloud sessions) is passed through."""
    args = ["docker", "run", "--rm", "-v", "%s:/src" % ROOT, "-w", workdir,
            "-e", "GIT_CONFIG_COUNT=1", "-e", "GIT_CONFIG_KEY_0=safe.directory", "-e", "GIT_CONFIG_VALUE_0=*"]
    if sys.stdin.isatty():
        args.append("-t")
    if hasattr(os, "getuid"):
        args += ["-u", "%d:%d" % (os.getuid(), os.getgid()), "-e", "HOME=/tmp"]
    proxy = os.environ.get("HTTPS_PROXY") or os.environ.get("https_proxy")
    if proxy:
        args += ["--network", "host", "-e", "HTTPS_PROXY=" + proxy, "-e", "https_proxy=" + proxy]
        for var in ("NO_PROXY", "no_proxy"):
            if var in os.environ:
                args += ["-e", "%s=%s" % (var, os.environ[var])]
        ca = os.environ.get("SSL_CERT_FILE") or os.environ.get("REQUESTS_CA_BUNDLE")
        if ca and os.path.isfile(ca):
            args += ["-v", "%s:/tmp/proxy-ca.crt:ro" % ca]
            for var in ("SSL_CERT_FILE", "REQUESTS_CA_BUNDLE", "NODE_EXTRA_CA_CERTS", "CURL_CA_BUNDLE"):
                args += ["-e", var + "=/tmp/proxy-ca.crt"]
    return args + [image]


def in_docker(steps, strict):
    if not docker_ok():
        sys.exit("Docker is not running (Docker Desktop on Windows and macOS)")
    try:
        run(["docker", "build", "-t", DEV_IMAGE, os.path.join(HERE, "dev")])
        run(docker_args(DEV_IMAGE, "/src") + ["python3", "tools/check.py"] + (["--strict"] if strict else []) + steps)
    except RuntimeError:
        sys.exit(1)


def main():
    p = argparse.ArgumentParser(description="Build and test locally what CI builds and tests.")
    p.add_argument("steps", nargs="*", metavar="step", help="steps to run (default: all)")
    p.add_argument("--list", action="store_true", help="list the steps")
    p.add_argument("--docker", action="store_true", help="run in the openesl-dev Docker image")
    p.add_argument("--strict", action="store_true", help="missing tools fail the step")
    a = p.parse_args()
    if a.list:
        for name, (_, needs) in STEPS.items():
            print("%-8s %s" % (name, needs))
        return
    unknown = [s for s in a.steps if s not in STEPS]
    if unknown:
        p.error("unknown step %s (see --list)" % ", ".join(unknown))
    if a.docker:
        return in_docker(a.steps, a.strict)

    add_tool_dirs()
    results = []
    for name in a.steps or STEPS:
        print("\n=== %s" % name, flush=True)
        t = time.time()
        try:
            STEPS[name][0]()
            results.append((name, "ok", ""))
        except Skip as e:
            results.append((name, "FAILED" if a.strict else "skipped", str(e)))
        except RuntimeError as e:
            results.append((name, "FAILED", str(e)))
        results[-1] += (time.time() - t,)

    print("\n=== Summary")
    for name, state, why, secs in results:
        print("%-8s %-8s %5.0fs  %s" % (name, state, secs, why))
    if any(r[1] == "FAILED" for r in results):
        sys.exit(1)


if __name__ == "__main__":
    main()
