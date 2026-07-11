#!/usr/bin/env python3
"""L0 coverage of the display-corruption pixel gate (`probes.green_band_ratio` / `video_corruption_verdict`).

Why this file exists. On 2026-07-10 VA-API hardware decode engaged, `corruptedVideoFrames` stayed 0,
and every video was painted with neon-green horizontal bands anyway — ANGLE's import of the decoded
surface was broken, invisible to the media pipeline (durable/hardware.md). The pixel gate in
`tests/deck/test_media.py::test_video_is_not_corrupt` is the check that would have caught it, but that
gate only runs with a Deck attached. Its detector is a pure function over PNG bytes, so it is checked
here on synthetic frames on every push — and, per F1, it is checked that it can BOTH fire on corruption
AND stay silent on a clean (including green-foliage-heavy) frame. A detector that cannot do both is not
a check; it is either a rubber stamp or a false alarm.
"""

import io
import os
import sys
import unittest

from PIL import Image

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "..", "deck"))

from lib import probes  # noqa: E402

W, H = 400, 100
NEON = (0, 255, 0)
BLACK = (0, 0, 0)
# A saturated-but-natural foliage green: bright, but red and blue are substantial, so it is NOT the
# corruption signature. This is the pixel most likely to be mis-flagged, so it is the one we pin.
FOLIAGE = (70, 150, 60)


def _png(img):
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def _rows(row_colors):
    """Build a W×H image from a list of H (color_a, color_b, frac_a) row specs: the leftmost
    frac_a of the row is color_a, the rest color_b."""
    img = Image.new("RGB", (W, H))
    px = img.load()
    for y, (ca, cb, frac_a) in enumerate(row_colors):
        cut = int(W * frac_a)
        for x in range(W):
            px[x, y] = ca if x < cut else cb
    return img


class TestGreenBandRatio(unittest.TestCase):
    def test_solid_neon_bands_are_detected(self):
        # Rows 20–39 and 60–79 are full-width neon green: 40 of 100 rows are bands.
        rows = []
        for y in range(H):
            band = (20 <= y < 40) or (60 <= y < 80)
            rows.append((NEON, NEON, 1.0) if band else (FOLIAGE, FOLIAGE, 1.0))
        ratio = probes.green_band_ratio(_png(_rows(rows)))
        self.assertAlmostEqual(ratio, 0.40, places=2)
        ok, reason, _ = probes.video_corruption_verdict(_png(_rows(rows)))
        self.assertFalse(ok, f"corruption not detected: {reason}")

    def test_all_neon_is_fully_corrupt(self):
        img = Image.new("RGB", (W, H), NEON)
        self.assertEqual(probes.green_band_ratio(_png(img)), 1.0)

    def test_solid_foliage_frame_is_clean(self):
        # A whole frame of natural green must NOT trip the gate — this is the false-positive guard.
        img = Image.new("RGB", (W, H), FOLIAGE)
        ratio = probes.green_band_ratio(_png(img))
        self.assertEqual(ratio, 0.0)
        ok, _reason, _ = probes.video_corruption_verdict(_png(img))
        self.assertTrue(ok)

    def test_black_frame_is_clean(self):
        img = Image.new("RGB", (W, H), BLACK)
        self.assertEqual(probes.green_band_ratio(_png(img)), 0.0)

    def test_sub_threshold_row_fraction_is_not_a_band(self):
        # Every row is 30% neon / 70% black — below the 40% row fraction, so NO row counts as a band.
        rows = [(NEON, BLACK, 0.30) for _ in range(H)]
        self.assertEqual(probes.green_band_ratio(_png(_rows(rows))), 0.0)

    def test_over_threshold_row_fraction_is_a_band(self):
        # 50% neon per row clears the 40% fraction: every row is a band.
        rows = [(NEON, BLACK, 0.50) for _ in range(H)]
        self.assertEqual(probes.green_band_ratio(_png(_rows(rows))), 1.0)

    def test_verdict_passes_just_under_and_fails_just_over_the_ratio(self):
        # 1 band row in 100 = 0.01 (<=0.02, pass); 3 band rows = 0.03 (>0.02, fail).
        def frame(n_bands):
            rows = [(NEON, NEON, 1.0) if y < n_bands else (BLACK, BLACK, 1.0) for y in range(H)]
            return _png(_rows(rows))

        ok1, _r1, ratio1 = probes.video_corruption_verdict(frame(1))
        self.assertTrue(ok1, f"1% should pass, ratio={ratio1}")
        ok3, _r3, ratio3 = probes.video_corruption_verdict(frame(3))
        self.assertFalse(ok3, f"3% should fail, ratio={ratio3}")

    def test_single_pixel_image_does_not_crash(self):
        img = Image.new("RGB", (1, 1), NEON)
        # One neon pixel in a 1×1 frame IS a band row by the math; the point is only that it returns.
        self.assertIsInstance(probes.green_band_ratio(_png(img)), float)


if __name__ == "__main__":
    unittest.main()
