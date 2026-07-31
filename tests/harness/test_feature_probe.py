#!/usr/bin/env python3
"""L0 coverage of `scripts/feature-probe.py`'s pure decision logic (P12.0a/P12.0b).

A probe's whole value is that its answer can be trusted enough to build on. These tests pin the
three places this one could print a confident answer it did not measure:

  * an empty method list is what a page with NO player produces, and also what a player exposing
    nothing would produce. The first must be ENV (3); only the second is a registerable negative.
  * on the browse screen the player exists but no media is loaded, so every quality getter answers
    `[]`/`"unknown"` and `<video>.playbackRate` is never reasserted. Both read as a clean positive
    for features that would then be built on nothing. `video_is_loaded()` is the guard.
  * zero pairing hits across screens nobody navigated to is not "our UA suppresses pairing".

The first two are the green-band lesson at the page layer: on m114 every automated metric passed and
a human saw corruption, because the checks could not fail. A negative finding is fine here; a
confident answer from no data is not.
"""

import importlib.util
import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_PROBE = os.path.join(_HERE, "..", "..", "scripts", "feature-probe.py")

# Hyphenated filename: not importable by name, and renaming it would rename the recipe.
_spec = importlib.util.spec_from_file_location("feature_probe", _PROBE)
fp = importlib.util.module_from_spec(_spec)
sys.modules["feature_probe"] = fp
_spec.loader.exec_module(fp)


def _dump(methods=(), video=None, found=True):
    return {"found": found, "methods": list(methods), "video": video}


PLAYING = {"duration": 212.3, "width": 1280, "height": 720, "paused": False, "currentTime": 4.0}
IDLE = {"duration": None, "width": 0, "height": 0, "paused": True, "currentTime": 0}


class TestClassifyMethods(unittest.TestCase):
    """Pattern-matched, not whitelisted — the point of a dump is to find unpredicted names."""

    def test_buckets_by_what_the_name_claims(self):
        got = fp.classify_methods(["setPlaybackRate", "getAvailableQualityLevels", "seekTo"])
        self.assertEqual(got["rate"], ["setPlaybackRate"])
        self.assertEqual(got["quality"], ["getAvailableQualityLevels"])
        self.assertEqual(got["other"], ["seekTo"])

    def test_a_name_matching_both_is_in_both(self):
        # Being forced into one bucket is how a real lever gets overlooked.
        got = fp.classify_methods(["setPlaybackRateQuality"])
        self.assertIn("setPlaybackRateQuality", got["rate"])
        self.assertIn("setPlaybackRateQuality", got["quality"])
        self.assertEqual(got["other"], [])

    def test_empty_in_empty_out(self):
        self.assertEqual(fp.classify_methods([]), {"rate": [], "quality": [], "other": []})


class TestVideoIsLoaded(unittest.TestCase):
    """The browse screen carries a player whose <video> never loaded anything."""

    def test_idle_browse_screen_element_is_not_loaded(self):
        self.assertFalse(fp.video_is_loaded(IDLE))

    def test_real_playback_is_loaded(self):
        self.assertTrue(fp.video_is_loaded(PLAYING))

    def test_missing_element_is_not_loaded(self):
        self.assertFalse(fp.video_is_loaded(None))
        self.assertFalse(fp.video_is_loaded({}))

    def test_a_loaded_but_unstarted_video_still_counts(self):
        # Duration known, nothing played yet: there IS media, so quality answers are meaningful.
        self.assertTrue(fp.video_is_loaded({"duration": 90.0, "width": 0, "currentTime": 0}))


class TestPlayerVerdict(unittest.TestCase):
    def test_no_player_is_env_and_never_a_negative_claim(self):
        code, head, lines = fp.player_verdict(_dump(found=False), {}, {})
        self.assertEqual(code, fp.EX_ENV)
        self.assertIn("INCONCLUSIVE", head)
        # It must not say the player lacks anything: it never saw a player.
        self.assertNotIn("NEITHER", head)

    def test_a_real_negative_is_a_successful_probe(self):
        # Exit 0: "this build has no rate or quality API" is a finding, not a regression.
        code, head, _ = fp.player_verdict(_dump(["seekTo"], PLAYING), {}, {"video": False})
        self.assertEqual(code, fp.EX_OK)
        self.assertIn("NEITHER", head)

    def test_rate_held_on_an_idle_element_is_not_reported_as_a_win(self):
        rate = {"video": True, "before": 1, "after": 1.25, "later": 1.25, "held": True,
                "settle_s": 3.0}
        _, _, lines = fp.player_verdict(_dump(["seekTo"], IDLE), {}, rate)
        body = " ".join(lines)
        self.assertIn("NO", body)
        self.assertIn("Re-run with a video actually playing", body)
        self.assertNotIn("is buildable on the element alone", body)

    def test_rate_held_during_playback_is_reported_as_a_win(self):
        rate = {"video": True, "before": 1, "after": 1.25, "later": 1.25, "held": True,
                "settle_s": 3.0}
        _, _, lines = fp.player_verdict(_dump(["seekTo"], PLAYING), {}, rate)
        self.assertIn("is buildable on the element alone", " ".join(lines))

    def test_a_snap_back_says_the_player_overrides(self):
        rate = {"video": True, "before": 1, "after": 1.25, "later": 1.0, "held": False,
                "settle_s": 3.0}
        _, _, lines = fp.player_verdict(_dump(["seekTo"], PLAYING), {}, rate)
        self.assertIn("snapped back", " ".join(lines))

    def test_empty_quality_answers_carry_the_not_loaded_caveat(self):
        calls = {"getAvailableQualityLevels": {"exists": True, "value": []}}
        _, _, lines = fp.player_verdict(_dump(["getAvailableQualityLevels"], IDLE), calls, {})
        self.assertIn("CAVEAT", " ".join(lines))

    def test_no_caveat_once_media_is_loaded(self):
        calls = {"getAvailableQualityLevels": {"exists": True, "value": ["hd720"]}}
        _, _, lines = fp.player_verdict(_dump(["getAvailableQualityLevels"], PLAYING), calls, {})
        self.assertNotIn("CAVEAT", " ".join(lines))

    def test_a_method_that_exists_but_threw_still_counts_as_present(self):
        # A setter called with no arguments throws; that proves the name exists, which is the point.
        calls = {"setPlaybackRate": {"exists": True, "threw": "boom"}}
        _, head, lines = fp.player_verdict(_dump(["setPlaybackRate"], PLAYING), calls, {})
        self.assertIn("setPlaybackRate", " ".join(lines))
        self.assertIn("RATE", head)


class TestPairingHits(unittest.TestCase):
    def test_finds_the_leanback_phrasing(self):
        self.assertEqual(fp.pairing_hits("Settings\nLink with TV code\nAbout"),
                         ["link with tv code"])

    def test_is_case_and_whitespace_insensitive(self):
        self.assertEqual(fp.pairing_hits("LINK  WITH\n TV   CODE"), ["link with tv code"])

    def test_dedupes_repeats(self):
        self.assertEqual(len(fp.pairing_hits("TV code ... TV code ... tv code")), 1)

    def test_unrelated_text_is_no_hit(self):
        self.assertEqual(fp.pairing_hits("Home Trending Subscriptions Library"), [])

    def test_empty_text_is_no_hit(self):
        self.assertEqual(fp.pairing_hits(""), [])
        self.assertEqual(fp.pairing_hits(None), [])


_MENU = "Settings\nLanguage\nLocation\nRestricted Mode\nLinked devices\nAbout\nHelp\nSign out\n" * 2

# What the watch screen really produced on 2026-07-31: 28 characters of innerText plus 32 icon
# aria-labels. The padding alone pushes it past any length threshold, which is why length is not the
# test — this fixture is the regression.
_WATCH_TEXT = "YouTube\n0:43 / 1:17:29\n" + "\n".join(
    f"button label {i}" for i in range(32)
)


def _screen(label, text, hash_="#/"):
    return {"label": label, "text": text, "hash": hash_}


class TestScreenIsSubstantive(unittest.TestCase):
    """The watch screen is ~28 characters of innerText: a video and no chrome."""

    def test_bare_watch_text_is_thin(self):
        self.assertFalse(fp.screen_is_substantive("YouTube\n0:43 / 1:17:29"))

    def test_a_menu_is_substantive(self):
        self.assertTrue(fp.screen_is_substantive(_MENU))

    def test_empty_and_whitespace_are_thin(self):
        self.assertFalse(fp.screen_is_substantive(""))
        self.assertFalse(fp.screen_is_substantive("   \n\t  "))
        self.assertFalse(fp.screen_is_substantive(None))


# The real browse screen at #/ on 2026-07-31: 1513 characters of recommendation feed plus the
# sidebar. Long, not the player, and not remotely where a pairing row would live -- the fixture for
# the third and last way this probe found to answer from the wrong screen.
_BROWSE_TEXT = (
    "Recommended\n1:17:30\nSome long video title here\nA Channel\n20K views • 5 hours ago\n"
    "25:48\nAnother video\nAnother Channel\n1K views • 7 hours ago\n"
    "Recently uploaded\n1:44:11\nA third video\nThird Channel\n3.1K views • 5 hours ago\n"
    "Search\nArtem G\nSearch\nHome\nShorts\nSubscriptions\nLibrary\nMusic\nPodcasts\nNews\n"
    "Gaming\nLive\nMovies & TV\n"
)


class TestCouldHostPairing(unittest.TestCase):
    """Length is not the test, and neither is 'not the player'."""

    def test_the_watch_screen_cannot_host_it_however_long(self):
        self.assertTrue(fp.screen_is_substantive(_WATCH_TEXT))  # long enough...
        self.assertFalse(fp.could_host_pairing(_screen("start", _WATCH_TEXT, "#/watch?v=9p58")))

    def test_the_browse_feed_cannot_host_it_either(self):
        # Substantive and not the player, yet a pairing row would never be on the feed.
        self.assertTrue(fp.screen_is_substantive(_BROWSE_TEXT))
        self.assertFalse(fp.is_watch_screen("#/"))
        self.assertFalse(fp.could_host_pairing(_screen("start", _BROWSE_TEXT, "#/")))

    def test_a_settings_screen_can(self):
        self.assertTrue(fp.could_host_pairing(_screen("settings", _MENU, "#/settings")))

    def test_a_lone_settings_sidebar_entry_is_not_a_settings_screen(self):
        # The browse sidebar does carry a "Settings" row; one marker must not be enough.
        one = _BROWSE_TEXT + "\nSettings\n"
        self.assertEqual(len(fp.settings_markers(one)), 1)
        self.assertFalse(fp.could_host_pairing(_screen("start", one, "#/")))

    def test_a_thin_menu_screen_still_cannot(self):
        self.assertFalse(fp.could_host_pairing(_screen("settings", "Settings", "#/settings")))

    def test_watch_hash_forms(self):
        for h in ("#/watch?v=x", "/watch?v=x", "watch?v=x", "#/WATCH?v=x"):
            self.assertTrue(fp.is_watch_screen(h), h)
        for h in ("#/", "#/settings", "", None, "#/browse"):
            self.assertFalse(fp.is_watch_screen(h), h)


class TestPairingVerdict(unittest.TestCase):
    def test_nothing_navigated_is_env_not_absence(self):
        # The touch-probe lesson: "no pairing screen" and "nobody looked" are the same zero.
        for screens in ([], [_screen("settings", "")], [_screen("settings", "   \n ")]):
            code, head, _ = fp.pairing_verdict(screens, [])
            self.assertEqual(code, fp.EX_ENV, screens)
            self.assertIn("INCONCLUSIVE", head)

    def test_only_the_watch_screen_is_env_not_a_negative(self):
        # The real run that motivated this: the probe printed a confident "NO PAIRING ENTRY" about a
        # video that was playing at the time, because aria-labels made it look substantive.
        code, head, lines = fp.pairing_verdict(
            [_screen("start", _WATCH_TEXT, "#/watch?v=9p58")], [])
        self.assertEqual(code, fp.EX_ENV)
        self.assertIn("INCONCLUSIVE", head)
        self.assertIn("start", " ".join(lines))
        self.assertNotIn("NO PAIRING", head)

    def test_the_browse_feed_is_env_not_a_negative(self):
        # The second real run: 1513 chars at '#/', zero hits, and the probe called it a dead end.
        code, head, _ = fp.pairing_verdict([_screen("start", _BROWSE_TEXT, "#/")], [])
        self.assertEqual(code, fp.EX_ENV)
        self.assertIn("INCONCLUSIVE", head)

    def test_text_without_a_hit_is_a_real_negative(self):
        code, head, lines = fp.pairing_verdict([_screen("settings", _MENU, "#/settings")], [])
        self.assertEqual(code, fp.EX_OK)
        self.assertIn("NO PAIRING", head)
        # And it must tell the reader how the negative could still be wrong.
        self.assertIn("Settings screen itself was one", " ".join(lines))

    def test_a_hit_decides_p12_3(self):
        code, head, _ = fp.pairing_verdict(
            [_screen("settings", _MENU + "\nLink with TV code", "#/settings")], ["LOUNGE_ID_TOKEN"])
        self.assertEqual(code, fp.EX_OK)
        self.assertIn("REACHABLE", head)

    def test_a_hit_counts_even_on_a_screen_that_could_not_host_it(self):
        # The gate is on the NEGATIVE, never the positive: seeing the phrase is seeing it.
        code, head, _ = fp.pairing_verdict(
            [_screen("start", "Link with TV code", "#/watch?v=x")], [])
        self.assertEqual(code, fp.EX_OK)
        self.assertIn("REACHABLE", head)

    def test_config_keys_alone_never_carry_the_verdict(self):
        # MDX/lounge keys in ytcfg say the build knows about pairing, not that a user can reach it.
        code, head, _ = fp.pairing_verdict(
            [_screen("settings", _MENU, "#/settings")], ["IS_MDX_INITIALIZED"])
        self.assertIn("NO PAIRING", head)
        self.assertEqual(code, fp.EX_OK)

    def test_blank_screens_do_not_dilute_a_real_one(self):
        code, head, _ = fp.pairing_verdict(
            [_screen("start", ""), _screen("settings", _MENU + "\nLink with TV code", "#/settings")],
            [])
        self.assertIn("REACHABLE", head)
        self.assertEqual(code, fp.EX_OK)


_DRM_UNDOCKED = (
    "/sys/class/drm/card1-eDP-1/status:connected\n"
    "/sys/class/drm/card1-DP-1/status:disconnected\n"
    "/sys/class/drm/card1-HDMI-A-1/status:disconnected\n"
)
_DRM_DOCKED = (
    "/sys/class/drm/card1-eDP-1/status:connected\n"
    "/sys/class/drm/card1-DP-1/status:disconnected\n"
    "/sys/class/drm/card1-HDMI-A-1/status:connected\n"
)


def _snap(drm, geometry, w, h):
    return {
        "drm": fp.parse_drm_connectors(drm),
        "geometry": geometry,
        "page": {"innerWidth": w, "innerHeight": h, "dpr": 1, "screenWidth": w, "screenHeight": h},
    }


class TestDrmConnectors(unittest.TestCase):
    def test_parses_the_grep_output(self):
        got = fp.parse_drm_connectors(_DRM_DOCKED)
        self.assertEqual(got["eDP-1"], "connected")
        self.assertEqual(got["HDMI-A-1"], "connected")
        self.assertEqual(got["DP-1"], "disconnected")

    def test_the_internal_panel_is_not_an_external_output(self):
        # eDP-1 is the Deck's own screen and is always connected: counting it would make every
        # undocked run look docked, which is precisely the control this exists to be.
        self.assertEqual(fp.external_outputs(fp.parse_drm_connectors(_DRM_UNDOCKED)), [])
        self.assertEqual(fp.external_outputs(fp.parse_drm_connectors(_DRM_DOCKED)), ["HDMI-A-1"])

    def test_garbage_is_empty_not_an_exception(self):
        self.assertEqual(fp.parse_drm_connectors(""), {})
        self.assertEqual(fp.parse_drm_connectors(None), {})
        self.assertEqual(fp.parse_drm_connectors("no such file"), {})


class TestDockVerdict(unittest.TestCase):
    def test_nothing_plugged_in_is_env_not_a_finding(self):
        # "the dock changes nothing" and "nobody docked" are the same diff without the DRM control.
        before = _snap(_DRM_UNDOCKED, "1280 800", 1280, 800)
        after = _snap(_DRM_UNDOCKED, "1280 800", 1280, 800)
        code, head, _ = fp.dock_verdict(before, after)
        self.assertEqual(code, fp.EX_ENV)
        self.assertIn("INCONCLUSIVE", head)

    def test_a_resized_surface_means_a_resize_path(self):
        before = _snap(_DRM_UNDOCKED, "1280 800", 1280, 800)
        after = _snap(_DRM_DOCKED, "1920 1080", 1920, 1080)
        code, head, _ = fp.dock_verdict(before, after)
        self.assertEqual(code, fp.EX_OK)
        self.assertIn("SURFACE RESIZES", head)

    def test_gamescope_alone_moving_means_a_relaunch(self):
        before = _snap(_DRM_UNDOCKED, "1280 800", 1280, 800)
        after = _snap(_DRM_DOCKED, "1920 1080", 1280, 800)
        code, head, _ = fp.dock_verdict(before, after)
        self.assertEqual(code, fp.EX_OK)
        self.assertIn("relaunch", head.lower())

    def test_docked_with_no_movement_is_a_real_finding(self):
        before = _snap(_DRM_UNDOCKED, "1280 800", 1280, 800)
        after = _snap(_DRM_DOCKED, "1280 800", 1280, 800)
        code, head, lines = fp.dock_verdict(before, after)
        self.assertEqual(code, fp.EX_OK)
        self.assertIn("CHANGES NOTHING", head)
        # ...but it must not be read as "docking is unsupported" without looking at the TV: black,
        # mirrored-and-upscaled and nothing-at-all are three findings this reading cannot separate.
        self.assertIn("what the TV is physically showing", " ".join(lines))


if __name__ == "__main__":
    unittest.main(verbosity=2)
