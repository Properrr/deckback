#!/usr/bin/env python3
"""L0 coverage of the touch passthrough probe's pure logic (scripts/touch-probe.py).

The probe itself cannot run without a Deck AND a human tapping the panel, so without this file every
decision it makes would be unreviewable until the one session where it matters. What is pinned here
is the part that turns raw observations into a verdict — the part that would otherwise be able to
print a confident answer from no data at all, which is exactly the failure `just power` shipped once
(`mean 0.00 W … PASS` on a Deck with no battery telemetry).

The two that would bite:

  * `classify()` must rank a single `touchstart` above a thousand `mousemove`s. Only the former
    proves real `wl_touch` survived gamescope -> Xwayland -> XI2 -> Blink; the latter is the pointer
    emulation we already have and do not want. A count-based "whichever is biggest" would report
    'mouse' for a perfect passthrough sample and send the reader to the wrong design.
  * `verdict()` must return ENV, not a cheerful negative, when NOTHING was observed. "No touch events
    in mode 4" and "nobody tapped the screen" produce identical counts, and only one of them is a
    finding about gamescope.
"""

import importlib.util
import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_SRC = os.path.join(_HERE, "..", "..", "scripts", "touch-probe.py")


def _load_module():
    spec = importlib.util.spec_from_file_location("deckback_touch_probe", _SRC)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


tp = _load_module()


class ParseXpropCardinal(unittest.TestCase):
    def test_reads_the_value_xprop_actually_prints(self):
        self.assertEqual(tp.parse_xprop_cardinal("STEAM_TOUCH_CLICK_MODE(CARDINAL) = 4"), 4)
        self.assertEqual(tp.parse_xprop_cardinal("STEAM_TOUCH_CLICK_MODE(CARDINAL) = 0"), 0)

    def test_unset_is_none_not_zero(self):
        """`not found` means nobody has set the mode. Folding that to 0 would claim hover is held."""
        self.assertIsNone(tp.parse_xprop_cardinal("STEAM_TOUCH_CLICK_MODE:  not found."))

    def test_no_output_at_all_is_none(self):
        self.assertIsNone(tp.parse_xprop_cardinal(""))
        self.assertIsNone(tp.parse_xprop_cardinal(None))


class Classify(unittest.TestCase):
    def test_touch_outranks_a_flood_of_mouse_events(self):
        counts = {"touchstart": 1, "mousemove": 900, "pointermove": 400}
        self.assertEqual(tp.classify(counts), "touch")

    def test_pointer_outranks_mouse(self):
        self.assertEqual(tp.classify({"pointerdown": 2, "mousemove": 50}), "pointer")

    def test_hover_mode_reads_as_mouse_not_pointer(self):
        """Mode 0's signature: motion only, no press. It must not look like a usable press stream."""
        self.assertEqual(tp.classify({"mousemove": 45}), "mouse")

    def test_nothing_is_none(self):
        self.assertEqual(tp.classify({}), "none")


class SplitSides(unittest.TestCase):
    def test_buckets_presses_by_page_x(self):
        downs = [{"x": 100, "y": 0}, {"x": 1180, "y": 0}, {"x": 640, "y": 0}]
        self.assertEqual(tp.split_sides(downs, 1280), (1, 1, 1))

    def test_missing_coordinates_are_skipped_not_counted_as_left(self):
        self.assertEqual(tp.split_sides([{"x": None, "y": None}], 1280), (0, 0, 0))

    def test_zero_width_viewport_cannot_bucket_anything(self):
        self.assertEqual(tp.split_sides([{"x": 100, "y": 0}], 0), (0, 0, 0))


class Verdict(unittest.TestCase):
    def test_no_observations_is_env_not_a_finding(self):
        code, head, _ = tp.verdict({4: {"counts": {}}, 1: {"counts": {}}}, {})
        self.assertEqual(code, tp.EX_ENV)
        self.assertIn("INCONCLUSIVE", head)

    def test_touch_in_mode_4_is_the_passthrough_win(self):
        samples = {4: {"counts": {"touchstart": 6, "touchend": 6}, "maxTouches": 3}}
        code, head, lines = tp.verdict(samples, {"maxTouchPoints": 5})
        self.assertEqual(code, tp.EX_OK)
        self.assertIn("PASSTHROUGH WORKS", head)
        self.assertTrue(any("Multitouch confirmed" in ln for ln in lines))

    def test_single_point_passthrough_does_not_claim_multitouch(self):
        samples = {4: {"counts": {"touchstart": 6}, "maxTouches": 1}}
        _, head, lines = tp.verdict(samples, {"maxTouchPoints": 1})
        self.assertIn("PASSTHROUGH WORKS", head)
        self.assertFalse(any("Multitouch confirmed" in ln for ln in lines))
        self.assertTrue(any("Only ONE simultaneous point" in ln for ln in lines))

    def test_pointer_only_falls_back_and_names_the_flags(self):
        """A negative passthrough result is exit 0 — a finding, not a regression."""
        samples = {4: {"counts": {}}, 1: {"counts": {"pointerdown": 7, "click": 4}}}
        code, head, lines = tp.verdict(samples, {"maxTouchPoints": 0})
        self.assertEqual(code, tp.EX_OK)
        self.assertIn("DOES NOT REACH BLINK", head)
        body = "\n".join(lines)
        self.assertIn("--touch-events=enabled", body)
        self.assertIn("--touch-devices", body)

    def test_detected_touchscreen_suppresses_the_flag_hint(self):
        samples = {4: {"counts": {}}, 1: {"counts": {"pointerdown": 7}}}
        _, _, lines = tp.verdict(samples, {"maxTouchPoints": 5})
        self.assertNotIn("--touch-devices", "\n".join(lines))


if __name__ == "__main__":
    unittest.main(verbosity=2)
