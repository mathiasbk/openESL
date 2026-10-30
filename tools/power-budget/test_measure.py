import os
import tempfile
import unittest

import measure


def tag_log(sleep_ua=3.0, polls=10, interval_s=60.0, poll_ma=6.0, poll_ms=12.0, update_at=None):
    """A made-up log: sleep, a POLL every interval_s, and a display update (0.3 s at 7 mA,
    then 2.9 s at 3 mA) at update_at. Sampled every 0.1 ms while awake, every 0.1 s asleep."""
    pts = []
    t = 0.0

    def run(until, ua, step):
        nonlocal t
        while t < until - 1e-12:
            pts.append((t, ua))
            t += step
    for n in range(polls):
        run(n * interval_s + 1.0, sleep_ua, 0.1)
        run(t + poll_ms / 1000, poll_ma * 1000, 1e-4)
        if update_at is not None and n == update_at:
            run(t + 0.3, 7000.0, 1e-4)
            run(t + 2.9, 3000.0, 1e-3)
    run(polls * interval_s + 1.0, sleep_ua, 0.1)
    pts.append((t, sleep_ua))
    return pts


class MeasureTest(unittest.TestCase):
    def test_polls_and_sleep(self):
        r = measure.analyse(tag_log())
        self.assertEqual(r["polls"], 10)
        self.assertAlmostEqual(r["sleep_ua"], 3.0, places=2)
        self.assertAlmostEqual(r["poll_mc"], 0.072, places=3)   # 12 ms at 6 mA
        self.assertAlmostEqual(r["poll_ms"], 12.0, delta=0.2)
        self.assertAlmostEqual(r["poll_interval_s"], 60.0, delta=0.1)
        self.assertEqual(r["long"], [])
        # the average over the log is sleep plus the polls
        self.assertAlmostEqual(r["avg_ua"], 3.0 + 10 * 72.0 / r["duration_s"], places=1)

    def test_display_update(self):
        r = measure.analyse(tag_log(update_at=4))
        self.assertEqual(r["polls"], 9)
        self.assertEqual(len(r["long"]), 1)
        u = r["long"][0]
        # the POLL before it (0.072) + 0.3 s at 7 mA + 2.9 s at 3 mA
        self.assertAlmostEqual(u["mc"], 0.072 + 2.1 + 8.7, places=1)
        self.assertAlmostEqual(u["s"], 3.212, places=2)
        self.assertAlmostEqual(r["sleep_ua"], 3.0, places=2)

    def test_csv_units(self):
        # a PPK2 export: time in ms, current in uA, digital channels after them
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "ppk2.csv")
            with open(path, "w") as f:
                f.write("Timestamp(ms),Current(uA),D0-D7\n")
                for t, i in tag_log(polls=3):
                    f.write(f"{t * 1000:.4f},{i:.3f},00000000\n")
            r = measure.analyse(measure.samples(path))
            self.assertEqual(r["polls"], 3)
            self.assertAlmostEqual(r["sleep_ua"], 3.0, places=2)
            # no header: s and mA as told
            with open(path, "w") as f:
                for t, i in tag_log(polls=3):
                    f.write(f"{t:.6f},{i / 1000:.6f}\n")
            r = measure.analyse(measure.samples(path, "s", "mA"))
            self.assertEqual(r["polls"], 3)
            self.assertAlmostEqual(r["poll_mc"], 0.072, places=3)
            self.assertEqual(measure.main([path, "--current-unit", "mA", "--cells", "1"]), 0)

    def test_units_from_header(self):
        self.assertEqual(measure.unit_of("Timestamp(ms)", measure.TIME_UNITS), "ms")
        self.assertEqual(measure.unit_of("Current (µA)", measure.CURRENT_UNITS), "ua")
        self.assertIsNone(measure.unit_of("Current", measure.CURRENT_UNITS))

    def test_empty(self):
        with self.assertRaises(ValueError):
            measure.analyse([(0.0, 3.0)])


if __name__ == "__main__":
    unittest.main()
