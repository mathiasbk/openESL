#!/usr/bin/env python3
"""
openESL without hardware, in one command: the AP and a few tags in the simulator (sim/), the
esl-client bridge to it, and the web page with its dashboard connected to it.

  python3 tools/demo.py              build, start, and open http://localhost:8000/?demo#dashboard
  python3 tools/demo.py --docker     the same in Docker (Windows, or without a C compiler):
                                     the python:3.12 image with this checkout mounted

Options:
  --tags N          simulated tags (default 6, at most 16): every tag model in turn, each with
                    its own battery, temperature and signal; one is left without content
  --speed N         simulated seconds per real second (default 10: the tags poll every 6 s)
  --port N          the web page (default 8000)
  --bridge-port N   the bridge, the dashboard's address (default 8080)
  --no-browser      do not open the browser

Needs Python 3, make and a C compiler (Linux, macOS or WSL: the simulator speaks the AP's USB
serial protocol on a pseudo-terminal). Ctrl+C stops everything.

The page is the one in web/, copied into sim/build/demo-web/. The designer's previews need the
renderer (WebAssembly): it is taken from web/dist/wasm/ after "python3 web/build.py", or built
here when Emscripten (emcc) is on PATH. The dashboard works without it.

What runs, step by step (sim/README.md, tools/esl-client/README.md, web/README.md):
  make -C sim build/<model>/esl-sim                 one simulator per tag model
  sim/build/154/esl-sim serve --tags N --sims ...   the AP core and the tags, on a pseudo-terminal
  esl_client.py --serial <pty> serve                the bridge, on http://localhost:8080
  python3 -m http.server                            the page, on http://localhost:8000
"""
import argparse
import functools
import http.server
import importlib.util
import os
import shutil
import subprocess
import sys
import threading
import time
import urllib.request
import webbrowser

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SIM = os.path.join(ROOT, "sim")
WEB_OUT = os.path.join(SIM, "build", "demo-web")
DOCKER_IMAGE = "python:3.12-bookworm"   # Python with gcc and make
sys.path.insert(0, os.path.join(HERE, "esl-client"))
sys.path.insert(0, os.path.join(ROOT, "shared"))
import esl_client  # noqa: E402
import models  # noqa: E402  (shared/models.json)

# What the demo tags show, in turn: shelf labels in a small shop, and a door sign. The last
# tag gets nothing, as a tag that was just switched on.
CONTENT = [
    esl_client.content("price", ["Whole milk 1 L", "24.90", "NOK 24.90/L"]),
    esl_client.content("price", ["Rye bread", "39.00", "NOK 52.00/kg"], highlight=True),
    esl_client.content("text", ["Meeting room 2", "Free until 14:00"], frame=True),
    esl_client.content("price", ["Coffee 500 g", "79.90", "NOK 159.80/kg"]),
    esl_client.content("price", ["Butter 500 g", "54.90", "NOK 109.80/kg"]),
    esl_client.content("text", ["Welcome", "openESL demo"]),
    esl_client.content("price", ["Apples", "29.90", "NOK 29.90/kg"], highlight=True),
    esl_client.content("price", ["Orange juice 1 L", "32.50", "NOK 32.50/L"]),
]


def log(msg):
    print("demo:", msg, flush=True)


def build_sims():
    """One esl-sim per tag model; the first is the one that runs the AP"""
    builds = [m["build"] for m in models.load()]
    if "154" in builds:   # the tag of "serve" as sim/README.md shows it
        builds.remove("154")
        builds.insert(0, "154")
    if not shutil.which("make"):
        sys.exit("make not found. On Windows use --docker (or WSL); on macOS: xcode-select --install")
    log("building the simulator (make -C sim)")
    targets = ["build/%s/esl-sim" % b for b in builds]
    subprocess.run(["make", "-s", "-C", SIM] + targets, check=True)
    return [os.path.join(SIM, t) for t in targets]


def start_sim(sims, n_tags, speed):
    # The AP's tag is the first model; the others take the models in turn, starting with the next
    others = sims[1:] + sims[:1]
    p = subprocess.Popen([sims[0], "serve", "--tags", str(n_tags), "--speed", str(speed), "--sims", ",".join(others)],
                         stdout=subprocess.PIPE, text=True)
    first = p.stdout.readline().split()
    if first[:1] != ["PTY"]:
        p.kill()
        sys.exit("the simulator did not start: %s" % " ".join(first))
    # its other lines (one per tag) are not needed; keep the pipe from filling up
    threading.Thread(target=lambda: [None for _ in p.stdout], daemon=True).start()
    return p, first[1]


def fill_tags(cli, n_tags, timeout_s=60):
    """Waits for every tag's first POLL, then gives all but the last one content"""
    end = time.monotonic() + timeout_s
    tags = []
    while time.monotonic() < end:
        tags = sorted(t["mac"] for t in cli.tags()["tags"])
        if len(tags) >= n_tags:
            break
        time.sleep(0.3)
    log("%d tags have reported to the AP" % len(tags))
    for i, mac in enumerate(tags[:-1] if len(tags) > 1 else tags):
        cli.set_tag(mac, CONTENT[i % len(CONTENT)])
    return tags


def build_page():
    """web/ as web/build.py copies it, with the renderer when there is one"""
    spec = importlib.util.spec_from_file_location("web_build", os.path.join(ROOT, "web", "build.py"))
    wb = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(wb)
    wb.DIST = WEB_OUT
    os.makedirs(WEB_OUT, exist_ok=True)
    wb.copy_static()
    dist = os.path.join(ROOT, "web", "dist")
    for extra in ("wasm", "templates"):
        dst = os.path.join(WEB_OUT, extra)
        shutil.rmtree(dst, ignore_errors=True)
        if os.path.isdir(os.path.join(dist, extra)):
            shutil.copytree(os.path.join(dist, extra), dst)
    if not os.path.isdir(os.path.join(WEB_OUT, "wasm")):
        if shutil.which(os.environ.get("EMCC", "emcc")):
            log("building the renderer (Emscripten)")
            for m in wb.MODELS:
                wb.build_wasm(m, os.path.join(WEB_OUT, "wasm"))
        else:
            log("no renderer (python3 web/build.py, or Emscripten on PATH): the designer shows no previews, "
                "the dashboard works")


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass


def serve_in_thread(server):
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def wait_for(url, timeout_s=300):
    end = time.monotonic() + timeout_s
    while time.monotonic() < end:
        try:
            with urllib.request.urlopen(url, timeout=2) as r:
                if r.status < 500:
                    return True
        except OSError:
            pass
        time.sleep(1)
    return False


def run_docker(args):
    """The demo in the python image, with the ports on this computer and the browser here"""
    if not shutil.which("docker"):
        sys.exit("docker not found: install Docker Desktop (https://www.docker.com/products/docker-desktop/)")
    cmd = ["docker", "run", "--rm", "-i", "-p", "%d:%d" % (args.port, args.port),
           "-p", "%d:%d" % (args.bridge_port, args.bridge_port), "-v", ROOT + ":/openesl", "-w", "/openesl",
           DOCKER_IMAGE, "python3", "tools/demo.py", "--no-browser", "--host", "0.0.0.0",
           "--tags", str(args.tags), "--speed", str(args.speed), "--port", str(args.port),
           "--bridge-port", str(args.bridge_port)]
    log(" ".join(cmd))
    p = subprocess.Popen(cmd)
    try:
        url = "http://localhost:%d/?demo=http://localhost:%d#dashboard" % (args.port, args.bridge_port)
        if wait_for("http://localhost:%d/api/tags" % args.bridge_port) and not args.no_browser:
            webbrowser.open(url)
        return p.wait()
    except KeyboardInterrupt:
        p.terminate()
        return p.wait()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[1].strip(),
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tags", type=int, default=6)
    ap.add_argument("--speed", type=float, default=10)
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--bridge-port", type=int, default=8080)
    ap.add_argument("--host", default="127.0.0.1", help=argparse.SUPPRESS)   # 0.0.0.0 in Docker
    ap.add_argument("--no-browser", action="store_true")
    ap.add_argument("--docker", action="store_true")
    args = ap.parse_args()
    if not 1 <= args.tags <= 16:
        ap.error("--tags: 1 to 16")
    if args.docker:
        return run_docker(args)
    if os.name == "nt":
        sys.exit("The simulator needs Linux or macOS (a pseudo-terminal). On Windows: "
                 "python tools/demo.py --docker, or run it in WSL.")

    sims = build_sims()
    build_page()
    sim, pty = start_sim(sims, args.tags, args.speed)
    servers = []
    try:
        log("simulated AP on %s" % pty)
        cli = esl_client.SerialClient(pty, timeout=10)
        fill_tags(cli, args.tags)
        servers.append(serve_in_thread(http.server.ThreadingHTTPServer(
            (args.host, args.bridge_port), esl_client.make_bridge(cli))))
        servers.append(serve_in_thread(http.server.ThreadingHTTPServer(
            (args.host, args.port), functools.partial(QuietHandler, directory=WEB_OUT))))
        url = "http://localhost:%d/?demo=http://localhost:%d#dashboard" % (args.port, args.bridge_port)
        log("bridge on http://localhost:%d, page on http://localhost:%d" % (args.bridge_port, args.port))
        log("open %s  (Ctrl+C stops the demo)" % url)
        if not args.no_browser:
            webbrowser.open(url)
        while sim.poll() is None:
            time.sleep(0.5)
        log("the simulator stopped (exit %s)" % sim.returncode)
        return 1
    except KeyboardInterrupt:
        return 0
    finally:
        sim.terminate()
        try:
            sim.wait(5)
        except subprocess.TimeoutExpired:
            sim.kill()
        for s in servers:
            s.shutdown()


if __name__ == "__main__":
    sys.exit(main())
