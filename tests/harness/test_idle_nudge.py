#!/usr/bin/env python3
"""L0 coverage of the idle-nudge helper's pure logic (scripts/idle-nudge.py).

The helper keeps the Deck awake during playback by jiggling a synthetic pointer, but ONLY while it
believes a video is playing. That belief comes from parsing logind's inhibitor list. When the parse
is wrong the helper goes silent and the screen dims then suspends mid-video — with *no error* on the
Deck, because "nothing to nudge" and "detection broke" look identical from the outside.

The regression this file pins down: the helper used to grep the human-readable `systemd-inhibit
--list` table for "playback active". That table truncates its WHY column to the terminal width, so a
SteamOS systemd bump that tightened truncation dropped the match — helper silent, screen sleeps. The
fix reads logind's ListInhibitors over D-Bus (busctl --json), whose reply is never truncated. These
tests assert the matcher's behavior on both an untruncated source (True) and a truncated table
(False, documenting exactly the failure that shipped), so a future refactor can't quietly reintroduce
either half. They import the module WITHOUT python-evdev installed (CI has none) — proving the evdev
dependency stayed lazy, which is the only reason this logic is testable off-Deck at all.
"""

import importlib.util
import os
import sys
import tempfile
import unittest
from unittest import mock

_HERE = os.path.dirname(os.path.abspath(__file__))
_SRC = os.path.join(_HERE, "..", "..", "scripts", "idle-nudge.py")


def _load_module():
    spec = importlib.util.spec_from_file_location("deckback_idle_nudge", _SRC)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


idle = _load_module()


# A realistic logind ListInhibitors reply (busctl --json=short) with Deckback's two inhibitors held.
# The WHY strings appear verbatim — this is the source the fix relies on.
BUSCTL_PLAYING = (
    '{"type":"a(ssssuu)","data":[['
    '["idle","Deckback","Deckback: playback active","block",1000,4242],'
    '["sleep","Deckback","Deckback: pause/checkpoint before suspend","delay",1000,4242],'
    '["sleep","NetworkManager","NetworkManager needs to turn off networks","delay",0,1767]'
    ']]}'
)

# Same, but no playback inhibitor (video paused / in a menu): only the delay checkpoint is held.
BUSCTL_IDLE = (
    '{"type":"a(ssssuu)","data":[['
    '["sleep","Deckback","Deckback: pause/checkpoint before suspend","delay",1000,4242]'
    ']]}'
)

# The human-readable table, WIDE — the marker survives.
TABLE_FULL = (
    "WHO      UID  USER PID  COMM     WHAT WHY                       MODE\n"
    "Deckback 1000 deck 4242 deckback idle Deckback: playback active block\n"
)

# The same table TRUNCATED to a narrow terminal — logind ellipsizes the WHY column and "active" is
# gone. This is byte-for-byte the shape that silently broke detection on the SteamOS bump.
TABLE_TRUNCATED = (
    "WHO      UID  USER PID  COMM     WHAT WHY                 MODE\n"
    "Deckback 1000 deck 4242 deckback idle Deckback: playback… bloc\n"
)


class TestInhibitorMatch(unittest.TestCase):
    def test_matches_busctl_json_when_playing(self):
        self.assertTrue(idle.inhibitor_active(BUSCTL_PLAYING))

    def test_no_match_in_busctl_json_when_idle(self):
        self.assertFalse(idle.inhibitor_active(BUSCTL_IDLE))

    def test_matches_untruncated_text_table(self):
        self.assertTrue(idle.inhibitor_active(TABLE_FULL))

    def test_regression_truncated_table_loses_the_marker(self):
        # This is the bug, frozen: the marker is really absent once the column is truncated. It is
        # NOT that the matcher is wrong — it's that the truncatable table is the wrong *source*, which
        # is why read_inhibitors() prefers the D-Bus reply above.
        self.assertFalse(idle.inhibitor_active(TABLE_TRUNCATED))

    def test_empty_and_none_are_safe(self):
        self.assertFalse(idle.inhibitor_active(""))
        self.assertFalse(idle.inhibitor_active(None))


class TestDeckbackPlaying(unittest.TestCase):
    def _with_source(self, source, text):
        return mock.patch.object(idle, "read_inhibitors", return_value=(source, text))

    def test_playing_true_from_busctl(self):
        with self._with_source("busctl", BUSCTL_PLAYING):
            self.assertTrue(idle.deckback_playing())

    def test_playing_false_when_idle(self):
        with self._with_source("busctl", BUSCTL_IDLE):
            self.assertFalse(idle.deckback_playing())

    def test_playing_false_when_source_unavailable(self):
        with self._with_source("unavailable", ""):
            self.assertFalse(idle.deckback_playing())


class TestResolveInterval(unittest.TestCase):
    def setUp(self):
        self._env = mock.patch.dict(os.environ, {}, clear=False)
        self._env.start()
        os.environ.pop("DECKBACK_NUDGE_INTERVAL", None)

    def tearDown(self):
        self._env.stop()

    def test_default_when_nothing_given(self):
        self.assertEqual(idle.resolve_interval(), idle.DEFAULT_INTERVAL)

    def test_cli_value_wins(self):
        os.environ["DECKBACK_NUDGE_INTERVAL"] = "40"
        self.assertEqual(idle.resolve_interval(20), 20)

    def test_env_used_when_no_cli(self):
        os.environ["DECKBACK_NUDGE_INTERVAL"] = "30"
        self.assertEqual(idle.resolve_interval(), 30)

    def test_clamped_below_the_sleep_floor(self):
        # Never allow an interval at/over the 60 s minimum sleep timer, however over-tuned.
        self.assertEqual(idle.resolve_interval(600), idle.MAX_INTERVAL)
        self.assertLess(idle.resolve_interval(600), 60)

    def test_clamped_above_a_silly_low_value(self):
        self.assertEqual(idle.resolve_interval(0), idle.MIN_INTERVAL)

    def test_non_numeric_is_ignored(self):
        os.environ["DECKBACK_NUDGE_INTERVAL"] = "banana"
        self.assertEqual(idle.resolve_interval("also-bad"), idle.DEFAULT_INTERVAL)


class TestMissingLimit(unittest.TestCase):
    """The self-removal grace period, in wall-clock terms.

    This is the bug that actually took the helper down on the OLED Deck: `just deck-install-dev` is a
    flatpak *uninstall* immediately followed by an install, so the app dir disappears on every deploy.
    With the old 3-poll (~75 s) debounce the watcher decided Deckback was gone, ran `disable --now`,
    and left the unit disabled — screen dimming mid-video from then on, silently.
    """

    def test_grace_is_long_enough_to_survive_a_reinstall(self):
        # Observed deploy gaps were ~9-15 s; even a slow rebuild between uninstall and install must
        # not trip this.
        for interval in (idle.MIN_INTERVAL, idle.DEFAULT_INTERVAL, idle.MAX_INTERVAL):
            seconds = idle.missing_limit(interval) * interval
            self.assertGreaterEqual(seconds, 600, f"grace too short at interval={interval}")

    def test_never_fewer_than_three_polls(self):
        self.assertGreaterEqual(idle.missing_limit(100000), 3)

    def test_zero_interval_does_not_divide_by_zero(self):
        self.assertGreaterEqual(idle.missing_limit(0), 3)


class TestDeckbackInstalled(unittest.TestCase):
    def test_detects_user_flatpak_dir(self):
        with tempfile.TemporaryDirectory() as home:
            app_dir = os.path.join(home, ".local", "share", "flatpak", "app", idle.APP)
            self.assertFalse(idle.deckback_installed(home=home))
            os.makedirs(app_dir)
            self.assertTrue(idle.deckback_installed(home=home))


class TestLazyEvdev(unittest.TestCase):
    def test_module_imported_without_evdev_binding(self):
        # The detection logic must be importable where python-evdev is absent (all of CI). If evdev
        # were imported at module scope this file would have errored on import above; assert too that
        # the module never bound an evdev name at top level, so a stray top-level import can't creep
        # back and re-break off-Deck testability.
        self.assertNotIn("evdev", vars(idle))
        self.assertNotIn("UInput", vars(idle))
        self.assertTrue(hasattr(idle, "_open_uinput"))


if __name__ == "__main__":
    unittest.main()
