import unittest

import power_budget as pb


class PowerBudgetTest(unittest.TestCase):
    def test_answered_poll_is_cheaper_than_a_miss(self):
        p = pb.defaults()
        self.assertLess(pb.per_poll_mc(p, True), pb.per_poll_mc(p, False))
        # 5 ms at 4 mA + 5 ms at 12 mA = 0.08 mC
        self.assertAlmostEqual(pb.per_poll_mc(p, True), 0.08)

    def test_backoff(self):
        p = pb.defaults()
        # 2 polls at 60 s, then 120, 240, 480, then 600 s for the rest of the day
        self.assertEqual(pb.lost_ap_polls_per_day(p), 6 + (86400 - 60 * 2 - 120 - 240 - 480) // 600)

    def test_t6_defaults(self):
        p = pb.defaults()
        b = pb.budget(p)
        self.assertAlmostEqual(b["avg_ua"], b["sleep_ua"] + b["poll_ua"] + b["epd_ua"])
        self.assertLess(b["avg_ua"], b["budget_ua"] / 2)
        self.assertGreater(b["years"], 1)
        # at the highest allowed sleep current the average is exactly half the budget
        p["sleep_ua"] = pb.max_sleep_ua(pb.defaults())
        b = pb.budget(p)
        self.assertAlmostEqual(b["avg_ua"] + b["self_ua"], b["budget_ua"] / 2)

    def test_cli(self):
        self.assertEqual(pb.main(["--cells", "1", "--sleep-ua", "6.5"]), 0)


if __name__ == "__main__":
    unittest.main()
