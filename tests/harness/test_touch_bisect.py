#!/usr/bin/env python3
"""L0 coverage of the touch bisect's verdict (scripts/touch_bisect_probe.py).

The bisect answers "does touch still reach the gesture router?" across engines and page scripts. Its
one way to be worse than useless is to report OK for a cell it never actually ran -- so the only
logic worth pinning is that "could not observe" is ENV (3), never OK (0).
"""
import importlib.util
import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "touch_bisect_probe", os.path.join(_HERE, "..", "..", "scripts", "touch_bisect_probe.py"))
tb = importlib.util.module_from_spec(_spec)
sys.modules["touch_bisect_probe"] = tb
_spec.loader.exec_module(tb)


class TestVerdict(unittest.TestCase):
    def test_all_taps_seen_is_ok(self):
        self.assertEqual(tb.verdict({"sequences": 3}, 3), tb.EX_OK)
        self.assertEqual(tb.verdict({"sequences": 9}, 3), tb.EX_OK)

    def test_fewer_taps_than_dispatched_is_a_regression(self):
        # This is the cell that would have caught the v0.0.9 symptom, had the cause been reachable.
        self.assertEqual(tb.verdict({"sequences": 0}, 3), tb.EX_ASSERT)
        self.assertEqual(tb.verdict({"sequences": 2}, 3), tb.EX_ASSERT)

    def test_nothing_measured_is_env_not_ok(self):
        for stats in (None, {}, {"sequences": None}, {"sequences": "3"}, {"emitted": 3}):
            self.assertEqual(tb.verdict(stats, 3), tb.EX_ENV, stats)


if __name__ == "__main__":
    unittest.main(verbosity=2)
