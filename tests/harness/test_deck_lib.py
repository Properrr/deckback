#!/usr/bin/env python3
"""L0 coverage of the L2 harness's pure helpers (`tests/deck/lib/`).

Why this file exists at all. With no Deck attached, the whole `tests/deck/` suite skips — and **a
suite that skips proves nothing**. Every helper it depends on would be untested, and the first time
anyone plugs in a Deck they would be debugging the harness and the product at once, unable to tell
which is lying.

So the parts that can be checked without hardware are checked here, and CI runs them on every push.
The two that have already bitten:

  * `struct input_event` is 24 bytes on x86-64. With struct's `=` prefix, `l` is **4** bytes, so the
    obvious `=llHHi` silently packs 16-byte events the kernel reads as garbage. The only symptom on
    hardware is "the device exists but nothing happens".
  * `parse_power_now("")` must raise, not return 0.0. The shell one-liner it replaces averaged an
    empty field to `mean 0.00 W` and printed `PASS` against a "<= 9 W" budget — a perfect score from
    zero data, on a Deck with no battery telemetry.
"""

import os
import socket
import struct
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "..", "deck"))

from lib import probes  # noqa: E402
from lib import ssh as sshlib  # noqa: E402
from lib import uinput as u  # noqa: E402


class TestIoctlEncoding(unittest.TestCase):
    """The hardcoded UI_* numbers must agree with the kernel's _IOC macro.

    A transposed digit here fails as EINVAL on hardware with no clue why, and only on hardware.
    """

    def test_ui_constants_match_the_ioc_macro(self):
        self.assertEqual(u.ioc(u.IOC_NONE, ord("U"), 1, 0), u.UI_DEV_CREATE)
        self.assertEqual(u.ioc(u.IOC_NONE, ord("U"), 2, 0), u.UI_DEV_DESTROY)
        self.assertEqual(u.ioc(u.IOC_WRITE, ord("U"), 100, 4), u.UI_SET_EVBIT)
        self.assertEqual(u.ioc(u.IOC_WRITE, ord("U"), 101, 4), u.UI_SET_KEYBIT)
        self.assertEqual(u.ioc(u.IOC_WRITE, ord("U"), 103, 4), u.UI_SET_ABSBIT)

    def test_evdev_read_ioctls(self):
        # EVIOCGNAME(256) and EVIOCGBIT(EV_ABS, 8), as the capability probe uses them.
        self.assertEqual(u.eviocgname(256), 0x81004506)
        self.assertEqual(u.eviocgbit(0, 8), u.ioc(u.IOC_READ, ord("E"), 0x20, 8))
        self.assertEqual(u.eviocgbit(u.EV_ABS, 8), u.ioc(u.IOC_READ, ord("E"), 0x23, 8))

    def test_directions_are_distinct(self):
        # A read ioctl used where a write is needed silently corrupts memory rather than erroring.
        self.assertNotEqual(
            u.ioc(u.IOC_READ, ord("E"), 0x90, 4), u.ioc(u.IOC_WRITE, ord("E"), 0x90, 4)
        )


class TestStructLayouts(unittest.TestCase):
    def test_input_event_is_24_bytes(self):
        self.assertEqual(len(u.pack_input_event(u.EV_KEY, u.BTN_SOUTH, 1)), u.INPUT_EVENT_SIZE)
        self.assertEqual(u.INPUT_EVENT_SIZE, 24)

    def test_input_event_round_trips(self):
        raw = u.pack_input_event(u.EV_ABS, u.ABS_RY, -32768)
        _, _, t, c, v = struct.unpack("=qqHHi", raw)
        self.assertEqual((t, c, v), (u.EV_ABS, u.ABS_RY, -32768))

    def test_uinput_user_dev_is_1116_bytes(self):
        self.assertEqual(len(u.pack_user_dev(u.gamepad_spec())), u.UINPUT_USER_DEV_SIZE)

    def test_axis_ranges_land_in_the_right_slots(self):
        spec = u.DeviceSpec("x", abs_axes={u.ABS_RY: (-32768, 32767, 3, 7)})
        raw = u.pack_user_dev(spec)
        fields = struct.unpack("=80s4HI" + "64i" * 4, raw)
        absmax = fields[6 : 6 + 64]
        absmin = fields[70 : 70 + 64]
        absfuzz = fields[134 : 134 + 64]
        absflat = fields[198 : 198 + 64]
        self.assertEqual(absmax[u.ABS_RY], 32767)
        self.assertEqual(absmin[u.ABS_RY], -32768)
        self.assertEqual(absfuzz[u.ABS_RY], 3)
        self.assertEqual(absflat[u.ABS_RY], 7)
        # Everything else stays zero: a stray range on an axis we never declared confuses consumers.
        self.assertEqual(absmax[u.ABS_X], 0)


class TestDeviceSpecValidation(unittest.TestCase):
    def test_ev_syn_is_never_enabled(self):
        # UI_DEV_CREATE rejects EV_SYN, and the failure surfaces much later as "the device exists
        # but nothing reads it". `ev_bits()` guarantees this by construction, so this test is the
        # only thing standing between us and a future edit that adds it back.
        for spec in (u.gamepad_spec(), u.touchscreen_spec(), u.DeviceSpec("k", keys=(u.BTN_SOUTH,))):
            self.assertNotIn(u.EV_SYN, spec.ev_bits())
            self.assertTrue(set(spec.ev_bits()) <= {u.EV_KEY, u.EV_ABS})

    def test_every_abs_axis_needs_a_full_absinfo(self):
        with self.assertRaises(ValueError):
            u.DeviceSpec("x", abs_axes={u.ABS_X: (0, 100)}).validate()

    def test_inverted_axis_range_is_rejected(self):
        with self.assertRaises(ValueError):
            u.DeviceSpec("x", abs_axes={u.ABS_X: (100, 0, 0, 0)}).validate()

    def test_axis_beyond_abs_cnt_is_rejected(self):
        # Would write past the end of absmax[64] and silently corrupt the struct.
        with self.assertRaises(ValueError):
            u.DeviceSpec("x", abs_axes={99: (0, 1, 0, 0)}).validate()

    def test_name_must_fit_the_kernel_field(self):
        with self.assertRaises(ValueError):
            u.DeviceSpec("n" * 80).validate()
        with self.assertRaises(ValueError):
            u.DeviceSpec("").validate()
        u.DeviceSpec("n" * 79).validate()  # the boundary is usable

    def test_gamepad_has_the_controls_the_launcher_reads(self):
        spec = u.gamepad_spec()
        for code in (u.BTN_SOUTH, u.BTN_EAST, u.BTN_THUMBL, u.BTN_THUMBR):
            self.assertIn(code, spec.keys)
        # The right stick and the analog triggers: the two capabilities whose existence on the real
        # Steam virtual pad is still unverified. A test pad missing them could never expose that.
        for axis in (u.ABS_RX, u.ABS_RY, u.ABS_Z, u.ABS_RZ, u.ABS_HAT0X, u.ABS_HAT0Y):
            self.assertIn(axis, spec.abs_axes)
        self.assertEqual(spec.abs_axes[u.ABS_X], (-32768, 32767, 0, 0))
        self.assertEqual(spec.abs_axes[u.ABS_Z], (0, 255, 0, 0))

    def test_touchscreen_matches_the_capability_fallback(self):
        spec = u.touchscreen_spec()
        for axis in (u.ABS_MT_SLOT, u.ABS_MT_POSITION_X, u.ABS_MT_POSITION_Y):
            self.assertIn(axis, spec.abs_axes)


class TestEventSequences(unittest.TestCase):
    def test_press_emits_two_reports(self):
        # A press and a release inside one SYN_REPORT is a zero-duration press; what a consumer makes
        # of it is undefined.
        ev = u.press(u.BTN_SOUTH)
        syns = [e for e in ev if e[0] == u.EV_SYN]
        self.assertEqual(len(syns), 2)
        self.assertEqual(ev[0], (u.EV_KEY, u.BTN_SOUTH, 1))
        self.assertEqual(ev[2], (u.EV_KEY, u.BTN_SOUTH, 0))

    def test_dpad_recenters(self):
        ev = u.dpad(0, 1)
        self.assertIn((u.EV_ABS, u.ABS_HAT0Y, 1), ev)
        self.assertIn((u.EV_ABS, u.ABS_HAT0Y, 0), ev)  # or the direction is held forever

    def test_tap_opens_and_closes_a_contact(self):
        ev = u.tap(100, 200)
        self.assertIn((u.EV_ABS, u.ABS_MT_TRACKING_ID, 1), ev)
        self.assertIn((u.EV_ABS, u.ABS_MT_TRACKING_ID, -1), ev)  # -1 closes it
        self.assertIn((u.EV_KEY, u.BTN_TOUCH, 1), ev)
        self.assertIn((u.EV_KEY, u.BTN_TOUCH, 0), ev)

    def test_encode_events_is_a_multiple_of_the_event_size(self):
        # A blob that is not a whole number of events makes the kernel reject the *whole* write with
        # EINVAL, not just the trailing fragment.
        blob = u.encode_events(u.press(u.BTN_SOUTH) + u.dpad(1, 0))
        self.assertEqual(len(blob) % u.INPUT_EVENT_SIZE, 0)
        # press: down, SYN, up, SYN = 4. dpad: two axes, SYN, two axes recentred, SYN = 6.
        self.assertEqual(len(blob) // u.INPUT_EVENT_SIZE, 4 + 6)

    def test_encoded_bytes_match_the_event_list(self):
        events = u.press(u.BTN_SOUTH)
        blob = u.encode_events(events)
        for i, (t, c, v) in enumerate(events):
            _, _, gt, gc, gv = struct.unpack(
                "=qqHHi", blob[i * u.INPUT_EVENT_SIZE : (i + 1) * u.INPUT_EVENT_SIZE]
            )
            self.assertEqual((gt, gc, gv), (t, c, v))


class TestRemotePrograms(unittest.TestCase):
    def test_remote_program_is_valid_python(self):
        src = u.remote_program(u.gamepad_spec(), u.press(u.BTN_SOUTH))
        compile(src, "<remote>", "exec")

    def test_remote_program_is_deterministic(self):
        a = u.remote_program(u.gamepad_spec(), u.dpad(0, 1))
        b = u.remote_program(u.gamepad_spec(), u.dpad(0, 1))
        self.assertEqual(a, b)

    def test_remote_program_exits_3_on_a_missing_uinput(self):
        # Exit 3 is ENV in the harness taxonomy (.internal/HARNESS.md). A missing udev rule is not a
        # product defect and must never be reported as one.
        src = u.remote_program(u.gamepad_spec())
        self.assertIn("sys.exit(3)", src)
        self.assertIn("/dev/uinput", src)

    def test_remote_program_validates_before_emitting(self):
        with self.assertRaises(ValueError):
            u.remote_program(u.DeviceSpec("x", abs_axes={u.ABS_X: (5, 5, 0, 0)}))

    def test_capabilities_program_is_valid_python(self):
        compile(u.capabilities_program(), "<caps>", "exec")

    def test_capabilities_program_reads_the_three_open_questions(self):
        src = u.capabilities_program()
        for needle in (str(u.eviocgbit(u.EV_ABS, 8)), str(u.eviocgbit(u.EV_FF, 16))):
            self.assertIn(needle, src)


class TestFindDevice(unittest.TestCase):
    DEVICES = [
        {"path": "/dev/input/event3", "name": "FTS3528:00 2808:1015", "vendor": "2808",
         "product": "1015", "ev": [1, 3], "abs": [u.ABS_MT_SLOT, u.ABS_MT_POSITION_X], "ff": []},
        {"path": "/dev/input/event10", "name": "Microsoft X-Box 360 pad", "vendor": "28de",
         "product": "11ff", "ev": [1, 3, u.EV_FF], "abs": [u.ABS_X, u.ABS_Y, u.ABS_RX, u.ABS_RY],
         "ff": [u.FF_RUMBLE]},
    ]

    def test_by_name(self):
        got = u.find_device(self.DEVICES, name_contains="x-box")  # case-insensitive
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0]["path"], "/dev/input/event10")

    def test_by_capability(self):
        got = u.find_device(self.DEVICES, needs_abs=[u.ABS_MT_SLOT])
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0]["vendor"], "2808")

    def test_no_match_is_empty_not_an_exception(self):
        self.assertEqual(u.find_device(self.DEVICES, name_contains="nonesuch"), [])

    def test_all_filters_must_hold(self):
        # A pad with ABS_RX but no EV_FF must not match a query asking for both.
        self.assertEqual(u.find_device(self.DEVICES, needs_abs=[u.ABS_MT_SLOT], needs_ev=[u.EV_FF]), [])


class TestPowerTelemetry(unittest.TestCase):
    """The bug this class exists to keep dead: a gate that cannot fail."""

    def test_empty_reading_raises_instead_of_reading_zero(self):
        for bad in ("", "   ", "\n", None, "-", "n/a", "abc"):
            with self.assertRaises(probes.NoTelemetry):
                probes.parse_power_now(bad)

    def test_zero_watts_is_a_valid_reading(self):
        # On mains the Deck genuinely draws 0 from the battery. "0" is a number; "" is not. That
        # distinction is the whole point of the parser.
        self.assertEqual(probes.parse_power_now("0"), 0.0)

    def test_microwatts_to_watts(self):
        self.assertAlmostEqual(probes.parse_power_now("8750000"), 8.75)
        self.assertAlmostEqual(probes.parse_power_now("  9000000\n"), 9.0)

    def test_negative_reading_is_not_telemetry(self):
        with self.assertRaises(probes.NoTelemetry):
            probes.parse_power_now("-1")

    def test_current_voltage_fallback(self):
        # 1.2 A x 7.5 V = 9.0 W
        self.assertAlmostEqual(probes.parse_current_voltage("1200000", "7500000"), 9.0)
        with self.assertRaises(probes.NoTelemetry):
            probes.parse_current_voltage("", "7500000")
        with self.assertRaises(probes.NoTelemetry):
            probes.parse_current_voltage("1200000", "0")

    def test_summarize_refuses_to_average_nothing(self):
        # `mean 0.00 W over 0 samples -> PASS` is the exact bug. An empty sample list must raise.
        with self.assertRaises(probes.NoTelemetry):
            probes.summarize_power([], 9.0)

    def test_summarize_verdicts(self):
        mean, peak, verdict = probes.summarize_power([8.0, 9.0, 10.0], 9.5)
        self.assertAlmostEqual(mean, 9.0)
        self.assertAlmostEqual(peak, 10.0)
        self.assertEqual(verdict, "PASS")
        _, _, verdict = probes.summarize_power([9.6], 9.5)
        self.assertEqual(verdict, "FAIL")

    def test_budget_boundary_passes(self):
        _, _, verdict = probes.summarize_power([9.0], 9.0)
        self.assertEqual(verdict, "PASS")


class TestHardwareDecodeVerdict(unittest.TestCase):
    def test_vaapi_platform_decoder_passes(self):
        ok, reason = probes.hardware_decode_verdict("VaapiVideoDecoder", True)
        self.assertTrue(ok)
        self.assertIn("Vaapi", reason)

    def test_older_accelerator_name_also_passes(self):
        ok, _ = probes.hardware_decode_verdict("VaapiVideoDecodeAccelerator", True)
        self.assertTrue(ok)

    def test_missing_values_are_not_a_pass(self):
        # A probe that never fired must never look like working hardware.
        self.assertFalse(probes.hardware_decode_verdict(None, True)[0])
        self.assertFalse(probes.hardware_decode_verdict("", True)[0])
        self.assertFalse(probes.hardware_decode_verdict("VaapiVideoDecoder", None)[0])

    def test_a_missing_probe_and_a_false_flag_give_different_reasons(self):
        # "the Media domain never reported kIsPlatformVideoDecoder" is a broken probe; "the decoder
        # says it is not a platform decoder" is a product fact. Whoever reads the failure has to fix
        # different things, so the two must not collapse into one message.
        _, missing = probes.hardware_decode_verdict("VaapiVideoDecoder", None)
        _, false_flag = probes.hardware_decode_verdict("VaapiVideoDecoder", False)
        self.assertNotEqual(missing, false_flag)
        self.assertIn("no kIsPlatformVideoDecoder", missing)
        self.assertIn("not a platform decoder", false_flag)

    def test_software_decoders_fail(self):
        for name in ("Dav1dVideoDecoder", "VpxVideoDecoder", "FFmpegVideoDecoder"):
            ok, reason = probes.hardware_decode_verdict(name, False)
            self.assertFalse(ok, name)
            self.assertIn("software", reason)

    def test_software_decoder_claiming_to_be_platform_still_fails(self):
        # Both conditions are load-bearing. If only `is_platform` were checked, a lying decoder
        # passes; if only the name were checked, a non-platform Vaapi path passes.
        ok, reason = probes.hardware_decode_verdict("Dav1dVideoDecoder", True)
        self.assertFalse(ok)
        self.assertIn("software", reason)

    def test_unknown_platform_decoder_that_is_not_vaapi_fails(self):
        ok, reason = probes.hardware_decode_verdict("SomeOtherPlatformDecoder", True)
        self.assertFalse(ok)
        self.assertIn("not VA-API", reason)


class TestCodecSupportParsing(unittest.TestCase):
    def test_parses_the_json_triple(self):
        self.assertEqual(probes.parse_codec_support('{"mse":false,"canPlay":""}'), (False, False))
        self.assertEqual(probes.parse_codec_support('{"mse":true,"canPlay":"probably"}'), (True, True))

    def test_empty_is_unknown_not_false(self):
        # "the probe did not run" and "the codec is unsupported" are different answers.
        self.assertEqual(probes.parse_codec_support(""), (None, None))

    def test_absent_apis_are_unknown_not_false(self):
        # A page with no MediaSource, or a <video> with no canPlayType, reports null. Reading that
        # as "unsupported" would make the AV1 assertion pass on a page where nothing was measured.
        self.assertEqual(probes.parse_codec_support('{"mse":null,"canPlay":null}'), (None, None))
        self.assertEqual(probes.parse_codec_support('{"mse":true,"canPlay":null}'), (True, None))

    def test_maybe_counts_as_supported(self):
        self.assertEqual(probes.parse_codec_support('{"mse":true,"canPlay":"maybe"}'), (True, True))

    def test_codec_expr_escapes_quotes(self):
        expr = probes.codec_support_expr("video/mp4; codecs='x'")
        self.assertIn("\\'", expr)


class TestSshHostname(unittest.TestCase):
    """DECK_HOST is an ssh destination, not a hostname.

    `reachable()` used to hand it straight to `socket.create_connection`, which cannot parse a
    `user@` prefix. It therefore answered False for every Deck ever configured. The suite above it
    had exactly three assertions, all `assertFalse` -- a function hardcoded to `return False` passed
    all of them. A check that cannot fail is not a check (F1), including in the tests.
    """

    def test_strips_the_user_prefix(self):
        self.assertEqual(sshlib.ssh_hostname("deck@192.168.1.10"), "192.168.1.10")

    def test_bare_hostname_is_unchanged(self):
        self.assertEqual(sshlib.ssh_hostname("192.168.1.10"), "192.168.1.10")
        self.assertEqual(sshlib.ssh_hostname("steamdeck.local"), "steamdeck.local")

    def test_ipv6_literal_is_unbracketed(self):
        self.assertEqual(sshlib.ssh_hostname("deck@[fe80::1]"), "fe80::1")
        self.assertEqual(sshlib.ssh_hostname("[::1]"), "::1")

    def test_an_at_sign_in_the_user_does_not_eat_the_host(self):
        # rsplit, not split: ssh takes the LAST '@' as the delimiter.
        self.assertEqual(sshlib.ssh_hostname("user@domain@10.0.0.5"), "10.0.0.5")

    def test_empty_forms_are_none(self):
        self.assertIsNone(sshlib.ssh_hostname(None))
        self.assertIsNone(sshlib.ssh_hostname(""))
        self.assertIsNone(sshlib.ssh_hostname("deck@"))


class TestSshHelpers(unittest.TestCase):
    def test_unreachable_host_is_false_not_an_exception(self):
        self.assertFalse(sshlib.reachable(None))
        self.assertFalse(sshlib.reachable("", 22))
        # 0.0.0.0:9 (discard) refuses fast on every platform we run on.
        self.assertFalse(sshlib.reachable("127.0.0.1", 9, timeout=0.5))

    def test_a_listening_port_is_reachable_through_a_user_at_destination(self):
        """The positive control the old suite lacked.

        This is the assertion that fails against the pre-fix `reachable()`: a real listening socket,
        addressed the way lib.sh actually builds DECK_HOST (`${STEAMDECK_USER}@${STEAMDECK_IP}`).
        """
        with socket.socket() as srv:
            srv.bind(("127.0.0.1", 0))
            srv.listen(1)
            port = srv.getsockname()[1]
            self.assertTrue(sshlib.reachable("127.0.0.1", port, timeout=2.0))
            self.assertTrue(sshlib.reachable("deck@127.0.0.1", port, timeout=2.0))

    def test_ssh_error_and_unreachable_are_different_types(self):
        # A retry decorator must be able to retry the transport failure and never the assertion.
        self.assertTrue(issubclass(sshlib.DeckUnreachable, ConnectionError))
        self.assertFalse(issubclass(sshlib.SshError, ConnectionError))

    def test_deck_host_prefers_the_environment(self):
        old = os.environ.get("DECK_HOST")
        os.environ["DECK_HOST"] = "deck.example"
        try:
            self.assertEqual(sshlib.deck_host(), "deck.example")
        finally:
            if old is None:
                del os.environ["DECK_HOST"]
            else:
                os.environ["DECK_HOST"] = old


class PlaybackAdvanced(unittest.TestCase):
    """`just power` and `just soak` are meaningless without this (T5).

    A 300-second battery average over a PAUSED video passes the 9 W budget effortlessly and describes
    nothing. It is the same defect as averaging an empty battery field to 0.00 W, one layer up: a
    measurement of the absence of the thing being measured.
    """

    def _st(self, t, **kw):
        base = {"video": True, "t": t, "paused": False, "ended": False, "ready": 4}
        base.update(kw)
        return base

    def test_advancing_playback_passes(self):
        ok, why = probes.playback_advanced(self._st(10.0), self._st(19.0), 10.0)
        self.assertTrue(ok, why)

    def test_a_frozen_currenttime_is_a_stall(self):
        ok, why = probes.playback_advanced(self._st(10.0), self._st(10.0), 10.0)
        self.assertFalse(ok)
        self.assertIn("stalled", why)

    def test_a_paused_video_never_passes(self):
        # THE bug this function exists to prevent. currentTime does not advance, and the naive read of
        # that is "stalled" — but the operator simply never pressed play, and calling it a product
        # defect sends the next person to debug the decoder.
        ok, why = probes.playback_advanced(self._st(10.0), self._st(10.0, paused=True), 10.0)
        self.assertFalse(ok)
        self.assertIn("paused", why)

    def test_backwards_is_reported_as_a_restart_not_a_stall(self):
        # Across a suspend/resume this is the "position correct" clause of the P6 gate failing. It is
        # not a stall, and describing it as one costs an hour of looking for a hang.
        ok, why = probes.playback_advanced(self._st(90.0), self._st(0.5), 10.0)
        self.assertFalse(ok)
        self.assertIn("BACKWARDS", why)
        self.assertNotIn("stalled", why)

    def test_no_video_element_is_not_a_pass(self):
        for before, after in (({"video": False}, self._st(1.0)), (self._st(1.0), {"video": False})):
            ok, why = probes.playback_advanced(before, after, 10.0)
            self.assertFalse(ok, why)
            self.assertIn("no <video>", why)

    def test_an_ended_video_is_not_a_pass(self):
        # It advanced, then it stopped. The remaining samples measure an idle menu.
        ok, why = probes.playback_advanced(self._st(10.0), self._st(19.0, ended=True), 10.0)
        self.assertFalse(ok)
        self.assertIn("ended", why)

    def test_a_slow_but_real_advance_passes_and_a_token_one_does_not(self):
        # min_fraction exists because a resume, a rebuffer, or an ad break legitimately costs a beat.
        # It must not become a licence to accept 0.1s of advance over 10s of wall clock.
        ok, _ = probes.playback_advanced(self._st(0.0), self._st(6.0), 10.0, min_fraction=0.5)
        self.assertTrue(ok)
        ok, why = probes.playback_advanced(self._st(0.0), self._st(0.4), 10.0, min_fraction=0.5)
        self.assertFalse(ok, why)

    def test_parse_playback_state_rejects_an_empty_reply(self):
        # An empty Runtime.evaluate result is "the probe broke", never "no video". json.loads("")
        # would raise something opaque; NoTelemetry says which of the two happened.
        with self.assertRaises(probes.NoTelemetry):
            probes.parse_playback_state("")
        self.assertEqual(probes.parse_playback_state('{"video":false}'), {"video": False})


class Epp(unittest.TestCase):
    """SteamOS resets EPP on cores 1-7 after resume (steamos#2383). We can measure it; root-only
    sysfs means we cannot fix it from the Flatpak. So `just soak` REPORTS and never gates."""

    def _grep(self, *pairs):
        return "\n".join(
            f"/sys/devices/system/cpu/cpu{c}/cpufreq/energy_performance_preference:{v}"
            for c, v in pairs
        )

    def test_parses_per_cpu_values(self):
        epp = probes.parse_epp(self._grep((0, "balance_performance"), (7, "performance")))
        self.assertEqual(epp, {0: "balance_performance", 7: "performance"})

    def test_no_nodes_raises_rather_than_reporting_an_empty_agreement(self):
        # {} == {} would make epp_report say "unchanged across all 0 cores", which is a perfect score
        # from zero data — F1 again.
        with self.assertRaises(probes.NoTelemetry):
            probes.parse_epp("")
        with self.assertRaises(probes.NoTelemetry):
            probes.parse_epp("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference:\n")

    def test_reports_the_known_regression(self):
        before = probes.parse_epp(self._grep(*[(c, "balance_performance") for c in range(8)]))
        after = probes.parse_epp(
            self._grep((0, "balance_performance"), *[(c, "performance") for c in range(1, 8)])
        )
        changed, summary = probes.epp_report(before, after)
        self.assertEqual(sorted(changed), list(range(1, 8)))
        self.assertIn("7/8", summary)
        self.assertIn("balance_performance->performance", summary)

    def test_unchanged_says_so(self):
        e = probes.parse_epp(self._grep((0, "power"), (1, "power")))
        changed, summary = probes.epp_report(e, e)
        self.assertEqual(changed, {})
        self.assertIn("unchanged", summary)

    def test_a_core_that_vanished_is_reported_not_intersected_away(self):
        # Silently intersecting the two readings would hide a core that stopped reporting, and
        # "fewer cores than before" is not something to learn about by not learning about it.
        before = probes.parse_epp(self._grep((0, "power"), (1, "power")))
        after = probes.parse_epp(self._grep((0, "power")))
        changed, _ = probes.epp_report(before, after)
        self.assertEqual(changed, {1: ("power", None)})


if __name__ == "__main__":
    unittest.main(verbosity=2)
