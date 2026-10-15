#!/usr/bin/env python3
"""power_budget.py - average current and battery life of a tag (requirement T6).

Adds up what one tag spends per day: asleep, awake for each POLL (one answered
exchange, or two listening windows when the AP does not answer), and each
display update. The timing comes from the firmware and from measurements on the
jig tag (docs/strombudsjett.md); the currents are estimates until phase 6
measures them, so every value can be changed on the command line:

  python3 tools/power-budget/power_budget.py                  the table in strombudsjett.md
  python3 tools/power-budget/power_budget.py --sleep-ua 6 --cells 1
  python3 tools/power-budget/power_budget.py --poll-s 120 --updates 4

Standard library only.
"""
import argparse
import sys

HOURS_PER_YEAR = 8760


def defaults():
    return {
        # timing: firmware (tag/src/config.h) and measurements on the jig tag
        "poll_s": 60,          # CFG_POLL_S, measured ~60 s between POLLs
        "max_poll_s": 600,     # CFG_MAX_POLL_S, backoff ceiling without an AP
        "backoff_after": 3,    # CFG_BACKOFF_AFTER
        "window_ms": 20,       # CFG_RX_TIMEOUT_MS, listening window after POLL
        "retry_pause_ms": 6,   # CFG_RETRY_MIN/MAX_MS = 2..10, mean
        "reply_ms": 5,         # POLL sent until the reply is in (AP answers in well under 5 ms, A3)
        "wake_ms": 5,          # clock start, radio on, frame built (estimate)
        "epd_busy_s": 2.9,     # measured: BUSY high ~2.9 s per refresh (kildelogg.md)
        "epd_init_s": 0.3,     # reset, init, writing 5 KB of image (estimate)
        "updates": 1,          # display updates per day (T6: one)
        # currents: estimates until phase 6
        "sleep_ua": 3.0,       # ZBS243 sleep timer + retained RAM + panel in deep sleep
        "cpu_ma": 4.0,         # 8051 core running at 16 MHz, radio off
        "radio_ma": 12.0,      # radio in RX or TX, core running
        "epd_ma": 3.0,         # panel during a refresh (charge pumps)
        # battery
        "cells": 1,            # CR2450 in parallel: 1 in the 1.54" tags, 2 in the 2.9" tags
        "cell_mah": 620,       # Panasonic CR2450 nominal (0.2 mA to 2.0 V)
        "usable": 0.7,         # derating: cut-off above 2.0 V, cold, pulse sag
        "self_discharge": 0.01,  # per year, of nominal capacity
    }


def per_poll_mc(p, answered):
    """Charge per POLL in millicoulombs (mA*s)."""
    awake = p["wake_ms"] * p["cpu_ma"]
    if answered:
        awake += p["reply_ms"] * p["radio_ma"]
    else:
        awake += 2 * p["window_ms"] * p["radio_ma"] + p["retry_pause_ms"] * p["cpu_ma"]
    return awake / 1000.0


def per_update_mc(p):
    t = p["epd_init_s"] + p["epd_busy_s"]
    return t * (p["cpu_ma"] + p["epd_ma"])  # the core waits on BUSY, it does not sleep


def lost_ap_polls_per_day(p):
    """Polls per day when the AP never answers: backoff_after misses at poll_s, then doubling to max_poll_s."""
    t, n, interval, misses = 0.0, 0, p["poll_s"], 0
    while t < 86400:
        n += 1
        misses += 1
        if misses >= p["backoff_after"]:
            interval = min(interval * 2, p["max_poll_s"])
        t += interval
    return n


def budget(p, ap_present=True):
    polls = 86400 / p["poll_s"] if ap_present else lost_ap_polls_per_day(p)
    sleep_mc = p["sleep_ua"] / 1000.0 * 86400
    poll_mc = polls * per_poll_mc(p, ap_present)
    epd_mc = p["updates"] * per_update_mc(p) if ap_present else 0.0
    total_mc = sleep_mc + poll_mc + epd_mc
    avg_ua = total_mc / 86400 * 1000
    nominal = p["cells"] * p["cell_mah"]
    self_ua = nominal * p["self_discharge"] / HOURS_PER_YEAR * 1000
    usable_mah = nominal * p["usable"]
    years = usable_mah / ((avg_ua + self_ua) / 1000) / HOURS_PER_YEAR
    return {
        "polls": polls,
        "sleep_ua": sleep_mc / 86.4,
        "poll_ua": poll_mc / 86.4,
        "epd_ua": epd_mc / 86.4,
        "avg_ua": avg_ua,
        "self_ua": self_ua,
        "usable_mah": usable_mah,
        "budget_ua": usable_mah / HOURS_PER_YEAR * 1000,
        "years": years,
    }


def table(p):
    rows = [("AP answers", budget(p, True)), ("AP gone all day", budget(p, False))]
    out = [f"{p['cells']} x CR2450, {p['usable'] * 100:.0f} % of {p['cells'] * p['cell_mah']} mAh usable; "
           f"poll every {p['poll_s']} s, {p['updates']} update(s) a day",
           f"{'case':18} {'polls/day':>9} {'sleep':>8} {'polls':>8} {'display':>8} {'average':>8} {'years':>6}"]
    for name, b in rows:
        out.append(f"{name:18} {b['polls']:9.0f} {b['sleep_ua']:6.2f}uA {b['poll_ua']:6.2f}uA "
                   f"{b['epd_ua']:6.2f}uA {b['avg_ua']:6.2f}uA {b['years']:6.1f}")
    b = rows[0][1]
    out.append(f"T6 budget (1 year): {b['budget_ua']:.0f} uA average, half of it {b['budget_ua'] / 2:.0f} uA; "
               f"self-discharge adds {b['self_ua']:.1f} uA")
    out.append(f"Sleep current that still keeps half the budget: {max_sleep_ua(p):.0f} uA")
    return "\n".join(out)


def max_sleep_ua(p):
    """The highest sleep current that keeps the average (with self-discharge) at half the T6 budget."""
    b = budget(p, True)
    return b["budget_ua"] / 2 - (b["avg_ua"] - b["sleep_ua"]) - b["self_ua"]


def main(argv=None):
    p = defaults()
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    for k, v in p.items():
        ap.add_argument("--" + k.replace("_", "-"), type=type(v), default=v)
    args = vars(ap.parse_args(argv))
    p.update(args)
    print(table(p))
    return 0


if __name__ == "__main__":
    sys.exit(main())
