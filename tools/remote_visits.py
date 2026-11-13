#!/usr/bin/env python3
"""Checks the AP's visits to channel 11 for the SoluM remote (ap/components/esl_core/include/esl_remote.h).

A press is a burst of ten frames: five about 10 ms apart, a pause of about 80 ms, five more.
The AP's radio visits channel 11 for VISIT ms every PERIOD ms. This tries every phase between
the two, for frames 8 to 12 ms apart and pauses of 60 to 100 ms, and says whether a visit
always covers a whole frame (0.6 ms on the air at 250 kbit/s).

    python3 tools/remote_visits.py            # the values in esl_remote.h
    python3 tools/remote_visits.py --search   # the lowest share of time away that never misses
"""
import re
import sys
from pathlib import Path

AIR_MS = 0.6


def burst(gap, pause):
    first = [i * gap for i in range(5)]
    return first + [first[-1] + pause + i * gap for i in range(5)]


def caught(period, visit, frames):
    for tenth in range(period * 10):
        start = tenth / 10
        starts = [start + k * period for k in range(-4, 12)]
        if not any(f >= s and f + AIR_MS <= s + visit for s in starts for f in frames):
            return False
    return True


def safe(period, visit):
    return all(caught(period, visit, burst(g, p)) for g in (8, 10, 12) for p in (60, 70, 80, 90, 100))


def main():
    if "--search" in sys.argv:
        best = min((v / p, v, p) for v in range(10, 25) for p in range(20, 130) if safe(p, v))
        print(f"visit {best[1]} ms every {best[2]} ms: away {best[0]:.0%}")
        return 0
    h = (Path(__file__).resolve().parent.parent / "ap/components/esl_core/include/esl_remote.h").read_text()
    period = int(re.search(r"ESL_REMOTE_PERIOD_MS\s+(\d+)", h).group(1))
    visit = int(re.search(r"ESL_REMOTE_VISIT_MS\s+(\d+)", h).group(1))
    ok = safe(period, visit)
    print(f"visit {visit} ms every {period} ms: {'never misses a press' if ok else 'MISSES presses'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
