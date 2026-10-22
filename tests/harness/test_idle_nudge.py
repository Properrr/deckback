#!/usr/bin/env python3
"""L0 coverage of the idle-nudge helper's pure logic (scripts/idle-nudge.py).

The helper nudges only while logind holds Deckback's "playback active" inhibitor, read from the
untruncated busctl --json reply. Imported without python-evdev, so evdev must stay lazy.
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


# A logind ListInhibitors reply (busctl --json=short) with Deckback's two inhibitors held.
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

# The same table TRUNCATED to a narrow terminal: the WHY column is ellipsized, "active" is gone.
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
        # The marker is really absent: the table is the wrong source, hence the D-Bus reply.
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
    """The self-removal grace period must outlast a reinstall (uninstall then install)."""

    def test_grace_is_long_enough_to_survive_a_reinstall(self):
        # Even a slow rebuild between uninstall and install must not trip this.
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


class TestHeartbeat(unittest.TestCase):
    """The liveness file the sandboxed app stats: under its $XDG_DATA_HOME, and never fatal."""

    def test_path_is_the_flatpak_data_dir(self):
        # The app reads $XDG_DATA_HOME/deckback/idle-nudge.alive, with ~/.var/app/<id>/data as
        # $XDG_DATA_HOME.
        self.assertEqual(
            idle.heartbeat_path(home="/home/deck"),
            "/home/deck/.var/app/io.github.properrr.deckback/data/deckback/idle-nudge.alive",
        )

    def test_write_creates_parents_and_refreshes_mtime(self):
        with tempfile.TemporaryDirectory() as home:
            path = os.path.join(home, "data", "deckback", "idle-nudge.alive")
            self.assertTrue(idle.write_heartbeat(path))
            self.assertTrue(os.path.exists(path))
            os.utime(path, (0, 0))
            self.assertTrue(idle.write_heartbeat(path))
            self.assertGreater(os.path.getmtime(path), 0)

    def test_write_failure_is_survivable(self):
        # Must return False, not raise: run_daemon() stamps every pass; a raise stops the nudging.
        with tempfile.TemporaryDirectory() as home:
            blocker = os.path.join(home, "blocked")
            with open(blocker, "w"):
                pass
            self.assertFalse(idle.write_heartbeat(os.path.join(blocker, "deckback", "hb")))


class TestLazyEvdev(unittest.TestCase):
    def test_module_imported_without_evdev_binding(self):
        # Importable without python-evdev, and no evdev name bound at module scope.
        self.assertNotIn("evdev", vars(idle))
        self.assertNotIn("UInput", vars(idle))
        self.assertTrue(hasattr(idle, "_open_uinput"))


if __name__ == "__main__":
    unittest.main()
