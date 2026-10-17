#!/usr/bin/env python3
"""
Small JSON settings file for the flash TUI: ~/.config/openesl/flash-tui.json
($XDG_CONFIG_HOME is respected, OPENESL_FLASH_TUI_CONFIG overrides the path).

The flasher port is remembered by its USB path (pyserial "location", e.g. "1-2.1:1.0"),
not by /dev/ttyUSBn, because the numbers swap when two CH340 boards are plugged in.
"""
import json
import os

import workflow


def default_path():
    if os.environ.get("OPENESL_FLASH_TUI_CONFIG"):
        return os.environ["OPENESL_FLASH_TUI_CONFIG"]
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "openesl", "flash-tui.json")


DEFAULTS = {
    "port_location": "",
    "port_device": "",
    "model": next(iter(workflow.MODELS)),
    "firmware": {k: m["firmware"] for k, m in workflow.MODELS.items()},
    "backup_dir": workflow.DEFAULT_BACKUP_DIR,
}


def load(path=None):
    path = path or default_path()
    cfg = json.loads(json.dumps(DEFAULTS))
    try:
        with open(path) as f:
            data = json.load(f)
    except (OSError, ValueError):
        return cfg
    for k in ("port_location", "port_device", "model", "backup_dir"):
        if isinstance(data.get(k), str):
            cfg[k] = data[k]
    if isinstance(data.get("firmware"), dict):
        for k in workflow.MODELS:
            if isinstance(data["firmware"].get(k), str) and data["firmware"][k]:
                cfg["firmware"][k] = data["firmware"][k]
    if cfg["model"] not in workflow.MODELS:
        cfg["model"] = next(iter(workflow.MODELS))
    return cfg


def save(cfg, path=None):
    path = path or default_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".part"
    with open(tmp, "w") as f:
        json.dump(cfg, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)


def find_remembered(cfg, ports):
    """The port that matches the remembered USB path (or device, if no USB path was known)."""
    if cfg.get("port_location"):
        for p in ports:
            if p["location"] == cfg["port_location"]:
                return p
        return None
    if cfg.get("port_device"):
        for p in ports:
            if p["device"] == cfg["port_device"] and not p["location"]:
                return p
    return None
