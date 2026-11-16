#!/usr/bin/env python3
"""scan.py - what a Bluetooth tag says about itself, from this PC.

  python3 tools/ble-probe/scan.py                    the BLE devices nearby, 10 s
  python3 tools/ble-probe/scan.py --name ESL         only those whose name has "ESL"
  python3 tools/ble-probe/scan.py --connect AA:BB:CC:DD:EE:FF
                                                     its services and characteristics,
                                                     and the values that can be read
  python3 tools/ble-probe/scan.py ... --json out.json

A scan lists each device's name, address, signal, maker data (manufacturer-specific data
by company id) and the service UUIDs it advertises. Run it once with the tag next to the
PC and once with it away to find which one it is. --connect reads its GATT table: the
first thing to know about a tag's protocol, before recording its app (btsnoop.py).

Only reads; it writes nothing to the tag. Needs bleak (pip install bleak) and a
Bluetooth adapter: Linux (BlueZ), Windows 10+ or macOS.
"""
import argparse
import asyncio
import json
import sys


def need_bleak():
    try:
        import bleak  # noqa: F401
    except ImportError:
        sys.stderr.write("scan.py needs bleak: python3 -m pip install bleak\n")
        sys.exit(2)


def adv_row(dev, adv):
    """One scanned device as plain data (also what --json writes)."""
    return {
        "address": dev.address,
        "name": adv.local_name or dev.name,
        "rssi": adv.rssi,
        "manufacturer": {"0x%04x" % k: bytes(v).hex() for k, v in (adv.manufacturer_data or {}).items()},
        "service_uuids": list(adv.service_uuids or []),
        "service_data": {k: bytes(v).hex() for k, v in (adv.service_data or {}).items()},
        "tx_power": adv.tx_power,
    }


def print_rows(rows, out=sys.stdout):
    for r in sorted(rows, key=lambda r: -(r["rssi"] if r["rssi"] is not None else -999)):
        out.write("%s  %4s dBm  %s\n" % (r["address"], r["rssi"], r["name"] or "(no name)"))
        for cid, data in r["manufacturer"].items():
            out.write("    maker %s: %s\n" % (cid, data))
        for u in r["service_uuids"]:
            out.write("    service %s\n" % u)
        for u, data in r["service_data"].items():
            out.write("    service data %s: %s\n" % (u, data))


async def scan(seconds, name):
    from bleak import BleakScanner
    found = await BleakScanner.discover(timeout=seconds, return_adv=True)
    rows = [adv_row(d, a) for d, a in found.values()]
    if name:
        rows = [r for r in rows if name.lower() in (r["name"] or "").lower()]
    return rows


async def gatt(address, read_values):
    from bleak import BleakClient
    out = {"address": address, "services": []}
    async with BleakClient(address) as client:
        out["mtu"] = getattr(client, "mtu_size", None)
        for s in client.services:
            srv = {"uuid": s.uuid, "handle": s.handle, "characteristics": []}
            for c in s.characteristics:
                ch = {"uuid": c.uuid, "handle": c.handle, "properties": list(c.properties),
                      "descriptors": [{"uuid": d.uuid, "handle": d.handle} for d in c.descriptors]}
                if read_values and "read" in c.properties:
                    try:
                        ch["value"] = bytes(await client.read_gatt_char(c)).hex()
                    except Exception as e:  # a tag may refuse a read until it is paired
                        ch["read_error"] = str(e)
                srv["characteristics"].append(ch)
            out["services"].append(srv)
    return out


def print_gatt(g, out=sys.stdout):
    out.write("%s%s\n" % (g["address"], ", MTU %s" % g["mtu"] if g.get("mtu") else ""))
    for s in g["services"]:
        out.write("  service %s\n" % s["uuid"])
        for c in s["characteristics"]:
            out.write("    0x%04x %s [%s]" % (c["handle"], c["uuid"], ",".join(c["properties"])))
            if "value" in c:
                out.write(" = %s" % c["value"])
            if "read_error" in c:
                out.write(" (read: %s)" % c["read_error"])
            out.write("\n")
            for d in c["descriptors"]:
                out.write("        0x%04x %s\n" % (d["handle"], d["uuid"]))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=10.0, help="how long to scan (default 10)")
    ap.add_argument("--name", help="only devices whose name contains this")
    ap.add_argument("--connect", metavar="ADDRESS", help="read this device's GATT table")
    ap.add_argument("--no-read", action="store_true", help="with --connect: list only, read no values")
    ap.add_argument("--json", metavar="FILE", help="also write the result to this file")
    args = ap.parse_args(argv)
    need_bleak()
    try:
        if args.connect:
            result = asyncio.run(gatt(args.connect, not args.no_read))
            print_gatt(result)
        else:
            result = asyncio.run(scan(args.seconds, args.name))
            print_rows(result)
    except Exception as e:
        sys.stderr.write("scan.py: %s\n" % e)
        return 1
    if args.json:
        with open(args.json, "w") as f:
            json.dump(result, f, indent=1)
            f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
