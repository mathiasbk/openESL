#!/usr/bin/env python3
"""measure.py - a tag's real current, from a log of a current meter, into the power budget.

Reads a CSV log of the tag's supply current (a Nordic Power Profiler Kit II export, or
any two columns of time and current), finds the sleep current between wake-ups and the
charge of each wake-up, and prints them as the measured figures of power_budget.py,
with the battery life they give:

  python3 tools/power-budget/measure.py ppk2-export.csv --cells 1
  python3 tools/power-budget/measure.py log.csv --time-unit s --current-unit mA

How to take the log: docs/strombudsjett.md, "Measuring a tag". Log at least a few
poll intervals (ten minutes at 60 s) with the tag on its AP; send it new content once
during the log to have a display update in it.

A wake-up is a stretch where the current is above --threshold-ua (default 100 uA; the
tag sleeps at a few uA and runs at mA), with gaps shorter than --merge-ms joined. A
short one (under --long-s) is a POLL; a long one is a display update, or an image or
firmware download, and all of those are listed. The sleep current is the average
outside the wake-ups. The file is read once, line by line, so a PPK2 log of millions
of samples fits.

Standard library only.
"""
import argparse
import csv
import re
import statistics
import sys

import power_budget as pb

TIME_UNITS = {"s": 1.0, "ms": 1e-3, "us": 1e-6}
CURRENT_UNITS = {"a": 1e6, "ma": 1e3, "ua": 1.0, "na": 1e-3}


def unit_of(header, units):
    """The unit in a column header such as "Timestamp(ms)" or "Current (uA)", or None."""
    m = re.search(r"\(\s*([a-zA-Zµμ]+)\s*\)", header)
    if not m:
        return None
    u = m.group(1).lower().replace("µ", "u").replace("μ", "u")
    return u if u in units else None


def samples(path, time_unit=None, current_unit=None):
    """(t in s, i in uA) for each row of the log. The first two numeric columns are time and
    current; their units come from the header (PPK2: "Timestamp(ms),Current(uA)") unless given."""
    with open(path, newline="", encoding="utf-8-sig") as f:
        rows = csv.reader(f)
        first = next(rows, None)
        if first is None:
            return
        tu, cu = time_unit, current_unit
        try:
            float(first[0])
            pending = [first]                  # no header: units as given, else s and uA
        except (ValueError, IndexError):
            pending = []
            if len(first) >= 2:
                tu = tu or unit_of(first[0], TIME_UNITS)
                cu = cu or unit_of(first[1], CURRENT_UNITS)
        ts, cs = TIME_UNITS[(tu or "s").lower()], CURRENT_UNITS[(cu or "ua").lower()]
        for row in pending + [r for r in rows]:
            if len(row) < 2:
                continue
            try:
                yield float(row[0]) * ts, float(row[1]) * cs
            except ValueError:
                continue


def analyse(points, threshold_ua=100.0, merge_ms=50.0, long_s=1.0):
    """Sleep current, wake-ups and the average over the log. points: (t s, i uA) in time order."""
    merge_s = merge_ms / 1000.0
    total_s = total_uc = 0.0           # whole log: time and charge (uA*s = uC)
    events = []                        # [start, last above, end of its charge, charge uC, peak uA]
    cur = None                         # the wake-up being read
    below_since = None                 # time the current fell under the threshold in it
    prev = None
    for t, i in points:
        if prev is not None:
            dt = t - prev[0]
            if dt <= 0:
                prev = (t, i)
                continue
            q = prev[1] * dt               # each sample holds until the next
            total_s += dt
            total_uc += q
            if cur is not None:
                cur[3] += q
                cur[2] = t
        if i >= threshold_ua:
            if cur is None:
                cur = [t, t, t, 0.0, i]
            cur[1] = t
            cur[4] = max(cur[4], i)
            below_since = None
        elif cur is not None:
            if below_since is None:
                below_since = t
            elif t - below_since >= merge_s:
                events.append(cur)
                cur = None
                below_since = None
        prev = (t, i)
    if cur is not None:
        events.append(cur)
    if total_s <= 0:
        raise ValueError("the log has fewer than two samples")
    # A wake-up's charge runs on through the dip that ends it (merge_ms); the sleep current is
    # the rest of the log, and each wake-up's cost is what it took on top of sleeping through it
    awake_s = sum(e[2] - e[0] for e in events)
    awake_uc = sum(e[3] for e in events)
    sleep_s = total_s - awake_s
    sleep_ua = (total_uc - awake_uc) / sleep_s if sleep_s > 0 else 0.0
    short = [e for e in events if e[1] - e[0] < long_s]
    long_ = [e for e in events if e[1] - e[0] >= long_s]

    def above_sleep(e):  # in mC
        return max(0.0, e[3] - sleep_ua * (e[2] - e[0])) / 1000.0

    return {
        "duration_s": total_s,
        "avg_ua": total_uc / total_s,
        "sleep_ua": sleep_ua,
        "polls": len(short),
        "poll_mc": statistics.median([above_sleep(e) for e in short]) if short else 0.0,
        "poll_ms": statistics.median([(e[1] - e[0]) * 1000 for e in short]) if short else 0.0,
        "poll_peak_ma": max((e[4] for e in short), default=0.0) / 1000,
        "poll_interval_s": (statistics.median([b[0] - a[0] for a, b in zip(short, short[1:])])
                            if len(short) > 1 else 0.0),
        "long": [{"start_s": e[0], "s": e[1] - e[0], "mc": above_sleep(e), "peak_ma": e[4] / 1000}
                 for e in long_],
    }


def report(r, p):
    out = [f"Log: {r['duration_s']:.1f} s, average {r['avg_ua']:.2f} uA",
           f"Sleep current: {r['sleep_ua']:.2f} uA (between wake-ups)"]
    if r["polls"]:
        out.append(f"POLLs: {r['polls']}, each {r['poll_mc'] * 1000:.0f} uC over {r['poll_ms']:.1f} ms "
                   f"(median), peak {r['poll_peak_ma']:.1f} mA"
                   + (f", every {r['poll_interval_s']:.0f} s" if r["poll_interval_s"] else ""))
    else:
        out.append("POLLs: none found (is --threshold-ua below the tag's current when awake?)")
    for e in r["long"]:
        out.append(f"Long wake-up at {e['start_s']:.1f} s: {e['s']:.2f} s, {e['mc']:.2f} mC, peak {e['peak_ma']:.1f} mA "
                   "(a display update, or an image or firmware download)")
    args = [f"--sleep-ua {r['sleep_ua']:.2f}"]
    if r["polls"]:
        args.append(f"--poll-mc {r['poll_mc']:.4f}")
    if r["long"]:
        args.append(f"--update-mc {min(e['mc'] for e in r['long']):.2f}")
        p["update_mc"] = min(e["mc"] for e in r["long"])
    p["sleep_ua"] = r["sleep_ua"]
    if r["polls"]:
        p["poll_mc"] = r["poll_mc"]
    out.append("")
    out.append("As measured figures: python3 tools/power-budget/power_budget.py " + " ".join(args))
    out.append(pb.table(p))
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("csv", help="the log: time and current, a header row optional (PPK2 export as it is)")
    ap.add_argument("--time-unit", type=str.lower, choices=sorted(TIME_UNITS), help="unit of the time column (default: from the header, else s)")
    ap.add_argument("--current-unit", type=str.lower, choices=sorted(CURRENT_UNITS), help="unit of the current column (default: from the header, else uA)")
    ap.add_argument("--threshold-ua", type=float, default=100.0, help="above this the tag is awake (default 100)")
    ap.add_argument("--merge-ms", type=float, default=50.0, help="shorter dips are part of the same wake-up (default 50)")
    ap.add_argument("--long-s", type=float, default=1.0, help="wake-ups this long or longer are not POLLs (default 1)")
    for k in ("cells", "poll_s", "updates"):
        ap.add_argument("--" + k.replace("_", "-"), type=int, default=pb.defaults()[k],
                        help="for the battery life (power_budget.py)")
    a = ap.parse_args(argv)
    try:
        r = analyse(samples(a.csv, a.time_unit, a.current_unit), a.threshold_ua, a.merge_ms, a.long_s)
    except (OSError, ValueError) as e:
        print(f"measure.py: {e}", file=sys.stderr)
        return 1
    p = pb.defaults()
    p.update(cells=a.cells, poll_s=a.poll_s, updates=a.updates)
    print(report(r, p))
    return 0


if __name__ == "__main__":
    sys.exit(main())
