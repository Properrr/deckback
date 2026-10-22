#!/usr/bin/env python3
"""L0 coverage of `scripts/deckctl.py`'s exit-code verdicts — no Deck, no browser, no socket.

    3 ENV     nothing is playing, the video is paused, the Media domain never reported
    2 ASSERT  a video is playing and the engine got it wrong (stalled, rewound, software decode)
"""

import argparse
import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "..", "..", "scripts"))
sys.path.insert(0, os.path.join(_HERE, "..", "deck"))

import deckctl  # noqa: E402


class FakeMedia:
    def __init__(self, name=None, is_platform=None):
        self.decoder_name = name
        self.is_platform_decoder = is_platform


class FakeCDP:
    """Answers the playback-state probe from a scripted list of states, one per evaluate()."""

    def __init__(self, states=(), media=None):
        self._states = list(states)
        self.media = media or FakeMedia()
        self.media_enabled = False

    def evaluate(self, _expr):
        import json

        return json.dumps(self._states.pop(0))

    def enable_media(self):
        self.media_enabled = True

    def pump(self, _seconds):
        return []


def _args(**kw):
    # A tiny but NON-ZERO window: with window=0 the required advance is 0 and a stall passes.
    base = dict(window=0.05, min_fraction=0.5, timeout=0.0, require_vaapi=False)
    base.update(kw)
    return argparse.Namespace(**base)


def _state(t, **kw):
    base = {"video": True, "t": t, "paused": False, "ended": False, "ready": 4}
    base.update(kw)
    return base


class Playing(unittest.TestCase):
    def test_advancing_playback_exits_zero(self):
        cdp = FakeCDP([_state(10.0), _state(20.0)])
        self.assertEqual(deckctl.cmd_playing(cdp, _args()), deckctl.EX_OK)

    def test_a_paused_video_is_ENV_not_a_product_failure(self):
        cdp = FakeCDP([_state(10.0, paused=True)])
        self.assertEqual(deckctl.cmd_playing(cdp, _args()), deckctl.EX_ENV)

    def test_no_video_is_ENV(self):
        cdp = FakeCDP([{"video": False}])
        self.assertEqual(deckctl.cmd_playing(cdp, _args()), deckctl.EX_ENV)

    def test_a_stall_is_ASSERT(self):
        # Playing, unpaused, currentTime frozen: a product failure, never ENV.
        cdp = FakeCDP([_state(10.0), _state(10.0)])
        self.assertEqual(deckctl.cmd_playing(cdp, _args()), deckctl.EX_ASSERT)

    def test_a_rewind_is_ASSERT(self):
        cdp = FakeCDP([_state(90.0), _state(0.0)])
        self.assertEqual(deckctl.cmd_playing(cdp, _args()), deckctl.EX_ASSERT)

    def test_pausing_mid_window_is_ENV_not_a_stall(self):
        cdp = FakeCDP([_state(10.0), _state(10.0, paused=True)])
        self.assertEqual(deckctl.cmd_playing(cdp, _args()), deckctl.EX_ENV)

    def test_a_video_that_ends_mid_window_is_ENV(self):
        cdp = FakeCDP([_state(10.0), _state(10.2, ended=True)])
        self.assertEqual(deckctl.cmd_playing(cdp, _args()), deckctl.EX_ENV)


class Decoder(unittest.TestCase):
    def test_vaapi_passes(self):
        cdp = FakeCDP(media=FakeMedia("VaapiVideoDecoder", True))
        self.assertEqual(deckctl.cmd_decoder(cdp, _args(require_vaapi=True)), deckctl.EX_OK)
        self.assertTrue(cdp.media_enabled, "Media.enable was never sent")

    def test_software_decode_is_ASSERT_when_required(self):
        cdp = FakeCDP(media=FakeMedia("Dav1dVideoDecoder", False))
        self.assertEqual(deckctl.cmd_decoder(cdp, _args(require_vaapi=True)), deckctl.EX_ASSERT)

    def test_software_decode_is_only_reported_when_not_required(self):
        cdp = FakeCDP(media=FakeMedia("Dav1dVideoDecoder", False))
        self.assertEqual(deckctl.cmd_decoder(cdp, _args(require_vaapi=False)), deckctl.EX_OK)

    def test_a_silent_media_domain_is_ENV_not_software_decode(self):
        # No decoder name means the probe never reported — not evidence of software decode.
        cdp = FakeCDP(media=FakeMedia(None, None))
        self.assertEqual(deckctl.cmd_decoder(cdp, _args(require_vaapi=True)), deckctl.EX_ENV)

    def test_a_missing_platform_flag_is_still_not_a_pass(self):
        cdp = FakeCDP(media=FakeMedia("VaapiVideoDecoder", None))
        self.assertEqual(deckctl.cmd_decoder(cdp, _args(require_vaapi=True)), deckctl.EX_ASSERT)


class Taxonomy(unittest.TestCase):
    def test_the_codes_match_harness_md(self):
        self.assertEqual(
            (deckctl.EX_OK, deckctl.EX_FAIL, deckctl.EX_ASSERT, deckctl.EX_ENV,
             deckctl.EX_TRANSPORT, deckctl.EX_USAGE),
            (0, 1, 2, 3, 4, 5),
        )

    def test_every_command_is_routed(self):
        # A command in `choices` with no handler would raise KeyError *after* opening a tunnel.
        routed = set(deckctl._NEEDS_CDP) | set(deckctl._NEEDS_SSH_ONLY)
        self.assertEqual(routed, {"playing", "current-time", "decoder", "epp"})
        self.assertFalse(set(deckctl._NEEDS_CDP) & set(deckctl._NEEDS_SSH_ONLY))


if __name__ == "__main__":
    unittest.main(verbosity=2)
