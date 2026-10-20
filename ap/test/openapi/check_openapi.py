#!/usr/bin/env python3
"""check_openapi.py - docs/openapi.yaml against the AP code.

Builds api_host (the AP's esl_api.c on the host) and checks that the spec and the code
say the same:
  - every example in the spec fits its schema, and so do the example layout files;
  - every request example gets 200 from the code, and every reply fits the schema the
    spec gives for that status, before and after the tags have polled;
  - wrong token, broken JSON, bad values, too large bodies, unknown MACs and models get
    a status the spec lists for the operation, with a reply that fits it;
  - a method the spec does not list gets 405, and every path in the code is in the spec.

  python3 ap/test/openapi/check_openapi.py        (needs a C compiler, make and PyYAML)
"""
import json
import os
import re
import subprocess
import sys

try:
    import yaml
except ImportError:
    sys.exit("check_openapi.py needs PyYAML (pip install pyyaml, or apt install python3-yaml)")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
SPEC = os.path.join(ROOT, "docs", "openapi.yaml")
API_C = os.path.join(ROOT, "ap", "components", "esl_core", "esl_api.c")
APP_MAIN = os.path.join(ROOT, "ap", "main", "app_main.c")
LAYOUTS = os.path.join(ROOT, "tag", "designs", "layouts")
AUTH = "Bearer s3cret"
METHODS = ("get", "put", "delete", "post")

errors = []


def fail(msg):
    errors.append(msg)


def tag_firmware_file(model=1, ver=300, size=1024):
    """A tag firmware file as tag/tools/build.py writes it (shared/proto.h ESL_FWF_*)."""
    import struct
    import zlib
    image = bytes((i * 7) & 0xFF for i in range(size))
    return struct.pack("<IBBHII", 0x5746534F, 1, model, ver, size, zlib.crc32(image)) + image


# ---- a small JSON Schema (2020-12) validator: what docs/openapi.yaml uses ----------------

class Validator:
    TYPES = {
        "object": lambda v: isinstance(v, dict),
        "array": lambda v: isinstance(v, list),
        "string": lambda v: isinstance(v, str),
        "integer": lambda v: isinstance(v, int) and not isinstance(v, bool),
        "number": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
        "boolean": lambda v: isinstance(v, bool),
        "null": lambda v: v is None,
    }
    KNOWN = {"type", "enum", "const", "properties", "required", "additionalProperties", "patternProperties",
             "unevaluatedProperties", "items", "minItems", "maxItems", "minimum", "maximum", "pattern",
             "minLength", "maxLength", "allOf", "oneOf", "anyOf", "not", "$ref",
             "description", "example", "examples", "default", "summary", "title"}

    def __init__(self, spec):
        self.spec = spec

    def resolve(self, s):
        while isinstance(s, dict) and "$ref" in s:
            node = self.spec
            for part in s["$ref"].lstrip("#/").split("/"):
                node = node[part]
            s = node
        return s

    def evaluated(self, s):
        """Property names and patterns this schema and its allOf members define."""
        s = self.resolve(s)
        names, pats = set(s.get("properties", {})), list(s.get("patternProperties", {}))
        for sub in s.get("allOf", []):
            n, p = self.evaluated(sub)
            names |= n
            pats += p
        return names, pats

    def check(self, v, s, where="$"):
        """A list of what is wrong with v against schema s."""
        if s is True:
            return []
        if s is False:
            return [f"{where}: not allowed"]
        out = []
        ref = s.get("$ref")
        if ref:
            out += self.check(v, self.resolve(s), where)
        unknown = set(s) - self.KNOWN
        if unknown:
            out.append(f"{where}: schema keyword(s) the checker does not know: {sorted(unknown)}")
        t = s.get("type")
        if t is not None:
            ts = t if isinstance(t, list) else [t]
            if not any(self.TYPES[x](v) for x in ts):
                return out + [f"{where}: {json.dumps(v)[:60]} is not {t}"]
        if "enum" in s and v not in s["enum"]:
            out.append(f"{where}: {json.dumps(v)} is not one of {s['enum']}")
        if "const" in s and v != s["const"]:
            out.append(f"{where}: {json.dumps(v)} is not {json.dumps(s['const'])}")
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            if "minimum" in s and v < s["minimum"]:
                out.append(f"{where}: {v} < {s['minimum']}")
            if "maximum" in s and v > s["maximum"]:
                out.append(f"{where}: {v} > {s['maximum']}")
        if isinstance(v, str):
            if "pattern" in s and not re.search(s["pattern"], v):
                out.append(f"{where}: {v!r} does not match {s['pattern']}")
            if "minLength" in s and len(v) < s["minLength"]:
                out.append(f"{where}: shorter than {s['minLength']}")
            if "maxLength" in s and len(v) > s["maxLength"]:
                out.append(f"{where}: longer than {s['maxLength']}")
        if isinstance(v, list):
            if "minItems" in s and len(v) < s["minItems"]:
                out.append(f"{where}: fewer than {s['minItems']} items")
            if "maxItems" in s and len(v) > s["maxItems"]:
                out.append(f"{where}: more than {s['maxItems']} items")
            if "items" in s:
                for i, x in enumerate(v):
                    out += self.check(x, s["items"], f"{where}[{i}]")
        if isinstance(v, dict):
            props = s.get("properties", {})
            pats = s.get("patternProperties", {})
            for k in s.get("required", []):
                if k not in v:
                    out.append(f"{where}: {k} is missing")
            for k, x in v.items():
                matched = False
                if k in props:
                    matched = True
                    out += self.check(x, props[k], f"{where}.{k}")
                for p, ps in pats.items():
                    if re.search(p, k):
                        matched = True
                        out += self.check(x, ps, f"{where}.{k}")
                if not matched and "additionalProperties" in s:
                    out += self.check(x, s["additionalProperties"], f"{where}.{k}")
            if s.get("unevaluatedProperties") is False:
                names, pl = self.evaluated(s)
                for k in v:
                    if k not in names and not any(re.search(p, k) for p in pl):
                        out.append(f"{where}.{k}: not in the schema")
        for sub in s.get("allOf", []):
            out += self.check(v, sub, where)
        if "anyOf" in s and all(self.check(v, sub, where) for sub in s["anyOf"]):
            out.append(f"{where}: matches none of anyOf")
        if "oneOf" in s:
            n = sum(1 for sub in s["oneOf"] if not self.check(v, sub, where))
            if n != 1:
                out.append(f"{where}: matches {n} of oneOf, not 1")
        if "not" in s and not self.check(v, s["not"], where):
            out.append(f"{where}: matches what it must not")
        return out


# ---- the API on the host ---------------------------------------------------------------

class Ap:
    def __init__(self, exe):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def _cmd(self, line, data=b""):
        self.p.stdin.write(line.encode() + b"\n" + data)
        self.p.stdin.flush()

    def req(self, method, uri, body=None, auth=AUTH):
        data = body if isinstance(body, bytes) else (b"" if body is None else json.dumps(body).encode())
        self._cmd(f"REQ {method.upper()} {uri} {auth.replace(' ', '+') if auth else '-'} {len(data)}", data)
        status, n = map(int, self.p.stdout.readline().split())
        raw = self.p.stdout.read(n + 1)[:n]
        try:
            return status, json.loads(raw)
        except ValueError:
            return status, raw

    def poll(self, mac, model, layout_ver, shown):
        self._cmd(f"POLL {mac} {model} {layout_ver if layout_ver is not None else '-'} {shown}")
        assert self.p.stdout.readline().strip() == b"OK"

    def close(self):
        self.p.stdin.close()
        self.p.wait()


def build():
    exe = os.path.join(ROOT, "ap", "test", "build", "api_host")
    subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "ap", "test"), "build/api_host"], check=True)
    return exe


# ---- the checks ------------------------------------------------------------------------------

def operations(spec):
    for path, item in spec["paths"].items():
        for m in METHODS:
            if m in item:
                params = item.get("parameters", []) + item[m].get("parameters", [])
                yield path, m, item[m], params


def json_schema(v, node):
    return v.resolve(node)["content"]["application/json"]["schema"]


def examples_of(media):
    if "example" in media:
        yield "example", media["example"]
    for name, ex in media.get("examples", {}).items():
        yield name, ex["value"]


def check_examples(spec, v):
    """Every example in the spec fits its schema."""
    def walk(node, where):
        if isinstance(node, dict):
            if "schema" in node:
                for name, ex in examples_of(node):
                    for e in v.check(ex, node["schema"], f"{where} {name}"):
                        fail(f"example does not fit: {e}")
            if "example" in node and "type" in node and "schema" not in node:
                for e in v.check(node["example"], node, where):
                    fail(f"example does not fit: {e}")
            for k, x in node.items():
                walk(x, f"{where}/{k}")
        elif isinstance(node, list):
            for i, x in enumerate(node):
                walk(x, f"{where}/{i}")
    walk(spec, "#")


def main():
    with open(SPEC) as f:
        spec = yaml.safe_load(f)
    v = Validator(spec)
    check_examples(spec, v)

    ap = Ap(build())

    def params_of(params):
        return {v.resolve(p)["name"]: v.resolve(p) for p in params}

    def call(path, method, op, params, body=None, auth=AUTH, values=None, expect=None, what=""):
        values = values or {}
        uri = path
        for name, p in params_of(params).items():
            uri = uri.replace("{%s}" % name, values.get(name, p.get("example", "")))
        status, reply = ap.req(method, uri, body, auth)
        label = f"{method.upper()} {uri}{' ' + what if what else ''}"
        if "x-host-status" in op and auth == AUTH:
            # the host build has no transport for this (OTA): it answers this status to everything
            expect = op["x-host-status"]
        responses = op["responses"]
        if str(status) not in responses:
            fail(f"{label}: the code answers {status}, which the spec does not list ({', '.join(responses)})")
            return status, reply
        if expect is not None and status not in (expect if isinstance(expect, tuple) else (expect,)):
            fail(f"{label}: expected {expect}, got {status}: {reply}")
        if not isinstance(reply, (dict, list)):
            fail(f"{label}: the reply is not JSON: {reply[:80]!r}")
            return status, reply
        for e in v.check(reply, json_schema(v, responses[str(status)]), f"{label} -> {status}"):
            fail(f"reply does not fit the spec: {e}")
        return status, reply

    ops = list(operations(spec))

    def op(path, method):
        return next((p, m, o, ps) for p, m, o, ps in ops if p == path and m == method)

    # the example layout files fit the schema and the code takes them
    layout_schema = spec["components"]["schemas"]["LayoutFile"]
    for name in sorted(os.listdir(LAYOUTS)):
        if not name.endswith(".json"):
            continue
        with open(os.path.join(LAYOUTS, name), "rb") as f:
            raw = f.read()
        doc = json.loads(raw)
        for e in v.check(doc, layout_schema, f"tag/designs/layouts/{name}"):
            fail(f"example layout file does not fit LayoutFile: {e}")
        call(*op("/api/layouts/{model}", "put"), body=raw, values={"model": doc["model"]}, expect=200, what=name)

    def run_all(methods, expect=200, image=(200, 409)):
        for path, m, o, params in ops:
            if m not in methods:
                continue
            body_ex = []
            rb = o.get("requestBody")
            if rb and "application/json" in rb["content"]:
                body_ex = list(examples_of(rb["content"]["application/json"]))
                if not body_ex:
                    fail(f"{m.upper()} {path}: no request example")
            elif rb and path == "/api/tag-firmware":
                body_ex = [("tag-29-ota.bin", tag_firmware_file())]
            values = {}
            # the image needs a tag of known model: 404 before the example tag polls, after
            # it 200, or 409 while the example layout file lacks the content's layout
            exp = image if path == "/api/tags/{mac}/image" else expect
            for name, ex in body_ex or [(None, None)]:
                if path == "/api/layouts/{model}" and ex:
                    values = {"model": ex["model"]}
                call(path, m, o, params, body=ex, values=values, expect=exp, what=name or "")

    # every request example is accepted, and every reply fits, before and after the tags poll
    run_all(("put",))
    run_all(("get",), image=404)
    _, reply = call(*op("/api/tags", "get"))
    st, layouts = call(*op("/api/layouts", "get"))
    ver29 = next(x["version"] for x in layouts["layouts"] if x["model"] == "2.9")
    for t in reply.get("tags", []):
        ap.poll(t["mac"], 1, 0, 0)                      # 2.9", layout files, nothing shown yet
    run_all(("get",))
    for t in reply.get("tags", []):
        ap.poll(t["mac"], 1, ver29, t["content_id"])    # shows its content, has the file
    ap.poll("0000000000000154", 2, None, 0)             # 1.54" without layout files
    run_all(("get",))

    # errors get a status the spec lists, with a reply that fits it
    for path, m, o, params in ops:
        call(path, m, o, params, auth=None, expect=401, what="without a token")
        call(path, m, o, params, auth="Bearer wrong", expect=401, what="with a wrong token")
        names = params_of(params)
        if "mac" in names:
            call(path, m, o, params, values={"mac": "123"}, expect=400, what="with a bad MAC",
                 body={} if m == "put" else None)
            if m != "put":
                call(path, m, o, params, values={"mac": "0123456789ABCDEF"}, expect=404, what="unknown tag")
        if "model" in names:
            call(path, m, o, params, values={"model": "4.2"}, expect=404, what="unknown model",
                 body=b"{}" if m == "put" else None)
        if "requestBody" in o and "application/json" in o["requestBody"]["content"]:
            call(path, m, o, params, body=b"", expect=400, what="without a body")
            call(path, m, o, params, body=b'{"a":', expect=400, what="broken JSON")
            call(path, m, o, params, body=b"[1]", expect=400, what="not an object")
            big = 8193 if "model" in names else 24577
            call(path, m, o, params, body=b" " * big, expect=413, what="too large")
            call(path, m, o, params, body={"poll_s": "x", "layout": 1, "tags": 1, "format": 1}, expect=422,
                 what="bad values")

    # deleting works, and then the item is gone
    run_all(("delete",))
    run_all(("delete",), expect=404)

    # a method the spec does not list gets 405; every path of the code is in the spec
    for path, item in spec["paths"].items():
        for m in METHODS:
            if m in item:
                continue
            uri = path
            for p in item.get("parameters", []):
                p = v.resolve(p)
                uri = uri.replace("{%s}" % p["name"], p["example"])
            status, reply = ap.req(m, uri, {} if m in ("put", "post") else None)
            if status != 405:
                fail(f"{m.upper()} {uri}: the spec does not list it, but the code answers {status}: {reply}")
    status, _ = ap.req("get", "/api/nothing")
    if status != 404:
        fail(f"GET /api/nothing: expected 404, got {status}")
    with open(API_C) as f:
        code_paths = re.findall(r'#define PATH_\w+ "(/api/[^"]*)"', f.read())
    for cp in code_paths:
        if not any(p == cp or p.startswith(cp + "/") for p in spec["paths"]):
            fail(f"{cp} is in esl_api.c but not in the spec")
    ap.close()

    # the reset reasons the AP can give are the ones the spec lists
    with open(APP_MAIN) as f:
        fn = re.search(r"reset_reason_name\(.*?\n}", f.read(), re.S).group(0)
    reasons = set(re.findall(r'return "(\w+)";', fn))
    listed = set(spec["components"]["schemas"]["ApInfo"]["properties"]["reset"]["enum"])
    if reasons != listed:
        fail(f"reset reasons: app_main.c has {sorted(reasons - listed)} the spec lacks, "
             f"the spec has {sorted(listed - reasons)} app_main.c lacks")

    for e in errors:
        print("FAIL", e)
    n_ops = len(ops)
    if errors:
        print(f"{len(errors)} problem(s) between docs/openapi.yaml and the AP code")
        return 1
    print(f"docs/openapi.yaml matches the AP code: {n_ops} operations, {len(code_paths)} paths")
    return 0


if __name__ == "__main__":
    sys.exit(main())
