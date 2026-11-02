"""
The tag model registry (shared/models.json) for the Python tools in this repository.

  import models                      (with shared/ on sys.path)
  models.load()                      -> the models OpenESL builds firmware for, ordered by code
  models.load(planned=True)          -> also the "planned" ones, which are only described
  models.by_build("154")             -> the model built with MODEL=154
  models.by_name("1.54")             -> the model the API calls "1.54"
  models.fw_dir(m)                   -> the build folder under tag/build/ ("fw", "fw154")
  models.layout_name(m)              -> the model whose layout file m uses ("2.9"): same_as, or its own name
  models.label(m)                    -> the model as people read it: '2.9"', '2.9" red (experimental)'
  python shared/models.py builds     -> "29 154" (for Makefiles and CI)
  python shared/models.py names      -> "29 2.9" and "154 1.54", one line each (CI)

Fields are described in shared/README.md. tools/gen_models.py checks the file.
"""
import json
import os

PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "models.json")


def load(path=PATH, planned=False):
    with open(path, encoding="utf-8") as f:
        ms = json.load(f)["models"]
    built = [m for m in ms if m["status"] != "planned"]
    for m in built:
        m["mac_code"] = int(m["mac_code"], 16)
    for m in ms:
        m["has_red"] = "red" in m["colors"]
    built.sort(key=lambda m: m["code"])
    return built + [m for m in ms if m["status"] == "planned"] if planned else built


def by_build(build, ms=None):
    for m in ms or load():
        if m["build"] == str(build):
            return m
    return None


def by_name(name, ms=None):
    for m in ms or load():
        if m["name"] == name:
            return m
    return None


def fw_dir(m):
    # The first model keeps the folder it always had (CI, tools/check.py and the flash-tui use it)
    return "fw" if m["code"] == 1 else "fw" + m["build"]


def layout_name(m):
    # A model with same_as uses that model's layout file, on the AP and for the example file
    # (tag/designs/layouts/<name>.json)
    return m.get("same_as", m["name"])


def label(m):
    # "short" in models.json, else the name and a ", with "(experimental)" for a model not
    # tried on a real tag yet (as modelLabel in web/js/models.js)
    s = m.get("short", m["name"] + '"')
    return s + " (experimental)" if m["status"] == "experimental" else s


if __name__ == "__main__":
    # python shared/models.py builds  -> "29 154", for Makefiles and shell scripts
    # python shared/models.py names   -> one "<build> <name>" line per model ("29 2.9")
    import sys
    if sys.argv[1:] == ["builds"]:
        print(" ".join(m["build"] for m in load()))
    elif sys.argv[1:] == ["names"]:
        print("\n".join("%s %s" % (m["build"], m["name"]) for m in load()))
    else:
        sys.exit("usage: models.py builds|names")
