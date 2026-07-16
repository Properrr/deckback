#!/usr/bin/env python3
"""L0 tests for scripts/steam_shortcuts.py `add` — the binary-VDF write path behind the one-line
installer (findings/durable/one-line-install.md).

The load-bearing invariant: `add` writes an explicit `appid = grid_id(exe, appname)` so that `art`
(and Steam) name the grid files with the SAME id — without that, artwork applied in the same run
can't match the tile. So we assert the round-trip AND `art_id(entry) == grid_id(exe, appname)`, plus
idempotency (re-running doesn't duplicate the tile) and fresh-file creation.

No Deck, no Steam. Run: tests/harness/test_steam_shortcuts.py
"""
import argparse
import importlib.util
import os
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_SPEC = importlib.util.spec_from_file_location(
    "steam_shortcuts", os.path.join(_HERE, "..", "..", "scripts", "steam_shortcuts.py"))
ss = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(ss)


def _add_args(vdf, **over):
    a = dict(appname="Deckback", exe="/usr/bin/flatpak",
             launch_options="run io.github.properrr.deckback", start_dir="/home/deck/",
             icon="", force=True, vdf=vdf)
    a.update(over)
    return argparse.Namespace(**a)


def _entries(vdf):
    _root, items = ss.load_shortcuts(vdf)
    return items


def _matching(vdf, appname="Deckback"):
    return [e for (_t, _k, e) in _entries(vdf) if ss._matches(e, appname)]


class Add(unittest.TestCase):
    def test_add_into_existing_empty_vdf_round_trips_with_a_crc_appid(self):
        with tempfile.TemporaryDirectory() as d:
            vdf = os.path.join(d, "shortcuts.vdf")
            ss.dump_shortcuts(vdf, "shortcuts", [])  # a valid, empty shortcuts.vdf
            self.assertEqual(0, ss.cmd_add(_add_args(vdf)))

            ms = _matching(vdf)
            self.assertEqual(len(ms), 1)
            entry = ms[0]
            self.assertEqual(ss._entry_field(entry, "AppName"), "Deckback")
            self.assertEqual(ss._entry_field(entry, "Exe"), "/usr/bin/flatpak")
            self.assertEqual(ss._entry_field(entry, "LaunchOptions"),
                             "run io.github.properrr.deckback")
            want_id = ss.grid_id("/usr/bin/flatpak", "Deckback")
            # The stored appid AND art_id() both resolve to grid_id — that is what makes the tile's
            # artwork land on the right files.
            self.assertEqual(ss._entry_field(entry, "appid"), want_id & 0xFFFFFFFF)
            self.assertEqual(ss.art_id(entry, "/usr/bin/flatpak", "Deckback"), want_id)

    def test_add_is_idempotent_no_duplicate_tiles(self):
        with tempfile.TemporaryDirectory() as d:
            vdf = os.path.join(d, "shortcuts.vdf")
            ss.dump_shortcuts(vdf, "shortcuts", [])
            ss.cmd_add(_add_args(vdf))
            ss.cmd_add(_add_args(vdf))
            self.assertEqual(len(_matching(vdf)), 1)

    def test_add_preserves_other_non_steam_games(self):
        with tempfile.TemporaryDirectory() as d:
            vdf = os.path.join(d, "shortcuts.vdf")
            other = ss._new_shortcut_entry(0x81000000, "Some Other Game", "/x", "/", "", "")
            ss.dump_shortcuts(vdf, "shortcuts", [(0x00, "0", other)])
            ss.cmd_add(_add_args(vdf))
            names = {ss._entry_field(e, "AppName") for (_t, _k, e) in _entries(vdf)}
            self.assertEqual(names, {"Some Other Game", "Deckback"})
            # entries are re-keyed 0..n-1
            self.assertEqual([k for (_t, k, _e) in _entries(vdf)], ["0", "1"])

    def test_add_creates_a_fresh_vdf_when_the_target_is_absent(self):
        with tempfile.TemporaryDirectory() as d:
            vdf = os.path.join(d, "config", "shortcuts.vdf")  # dir does not exist yet
            self.assertEqual(0, ss.cmd_add(_add_args(vdf)))
            self.assertTrue(os.path.isfile(vdf))
            self.assertEqual(len(_matching(vdf)), 1)

    def test_add_writes_a_backup_when_editing_an_existing_file(self):
        with tempfile.TemporaryDirectory() as d:
            vdf = os.path.join(d, "shortcuts.vdf")
            ss.dump_shortcuts(vdf, "shortcuts", [])
            ss.cmd_add(_add_args(vdf))
            self.assertTrue(os.path.isfile(vdf + ".deckback.bak"))


if __name__ == "__main__":
    unittest.main()
