#!/usr/bin/env python3
"""L0 tests for the pure parts of scripts/cdp.py — no socket, no engine, no Deck.

Covers `key_spec`, `MediaState` feeding `probes.hardware_decode_verdict` (the P4 gate), and the
`--assert`/`--baseline` machinery `just smoke` gates on.
"""
import os
import pathlib
import sys

_ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
sys.path.insert(0, os.path.join(_ROOT, "scripts"))
sys.path.insert(0, os.path.join(_ROOT, "tests", "deck"))
from cdp import (  # noqa: E402
    MOD_SHIFT, AV1_MIME, OK_MIMES, CDP, AssertionFailure, MediaState,
    av1_baseline_assertion, codec_assertions, key_spec, run_assertions,
)
from lib.probes import hardware_decode_verdict  # noqa: E402

_failures = []


def check(name, cond, detail=""):
    if cond:
        print(f"ok   {name}")
    else:
        print(f"FAIL {name}{': ' + detail if detail else ''}")
        _failures.append(name)


# ---- key_spec -------------------------------------------------------------------------------

def test_named_keys():
    check("ArrowUp resolves to VK 38", key_spec("ArrowUp") == ("ArrowUp", "ArrowUp", 38, ""))
    check("Enter resolves to VK 13", key_spec("Enter") == ("Enter", "Enter", 13, ""))
    # cobalt/src/starboard/key.h: kSbKeyMediaRewind = 0xE3, ..FastForward = 0xE4.
    check("MediaRewind is VK 227", key_spec("MediaRewind")[2] == 227)
    check("MediaFastForward is VK 228", key_spec("MediaFastForward")[2] == 228)
    # Delete(46) is the OSK erase candidate: kSbKeyBack == kSbKeyBackspace == 8, so Backspace is
    # predicted to pop the view rather than erase.
    check("Delete is VK 46", key_spec("Delete")[2] == 46)
    check("Backspace is VK 8", key_spec("Backspace")[2] == 8)
    check("aliases resolve (Down -> ArrowDown)", key_spec("Down") == key_spec("ArrowDown"))
    # Named keys carry no `text` -> dispatched as rawKeyDown, no char event.
    check("named keys carry no text", key_spec("ArrowLeft")[3] == "")


def test_printable_keys():
    # Printable keys MUST carry `text`, or Blink never fires beforeinput/textInput and nothing lands
    # in a focused <input>.
    check("'a' -> KeyA/65 with text", key_spec("a") == ("a", "KeyA", 65, "a"))
    check("'N' keeps the physical code", key_spec("N") == ("N", "KeyN", 78, "N"))
    check("'7' -> Digit7/55", key_spec("7") == ("7", "Digit7", 55, "7"))
    check("space -> Space/32", key_spec(" ") == (" ", "Space", 32, " "))
    # A shifted char reports the physical key it comes from: '?' lives on Slash.
    check("'?' -> Slash/191", key_spec("?") == ("?", "Slash", 191, "?"))
    check("'/' -> Slash/191 too", key_spec("/") == ("/", "Slash", 191, "/"))
    check("'!' -> Digit1/49", key_spec("!") == ("!", "Digit1", 49, "!"))


def test_refuses_to_guess():
    # An unknown name yields None so the caller raises rather than dispatching a dud key.
    for bad in ("VolumeUp", "ab", "", "\n", "\x7f", "PlayPause"):
        check(f"refuses {bad!r}", key_spec(bad) is None)


# ---- MediaState -----------------------------------------------------------------------------

def _props_event(pid, **props):
    return {"method": "Media.playerPropertiesChanged",
            "params": {"playerId": pid,
                       "properties": [{"name": k, "value": v} for k, v in props.items()]}}


def test_media_accumulates():
    m = MediaState()
    m.feed(_props_event("p1", kVideoDecoderName="VaapiVideoDecoder", kIsPlatformVideoDecoder="true"))
    check("decoder name captured", m.decoder_name == "VaapiVideoDecoder")
    check("platform flag parsed", m.is_platform_decoder is True)


def test_media_ignores_unrelated_events():
    m = MediaState()
    m.feed({"method": "Runtime.consoleAPICalled", "params": {"type": "log"}})
    m.feed({"id": 7, "result": {}})
    check("no players from unrelated messages", m.players == {})
    check("decoder unknown -> None", m.decoder_name is None)
    check("platform flag unknown -> None", m.is_platform_decoder is None)


def test_media_null_does_not_erase():
    """A null (re-emitted on player teardown) must not erase a decoder name already observed."""
    m = MediaState()
    m.feed(_props_event("p1", kVideoDecoderName="VaapiVideoDecoder"))
    m.feed(_props_event("p1", kVideoDecoderName=None))
    check("null does not erase the decoder name", m.decoder_name == "VaapiVideoDecoder")


# ---- the P4 gate itself (MediaState -> probes.hardware_decode_verdict) ----------------------

def _verdict(m):
    """Exactly how `tests/deck/test_media.py` reads it: two properties, one adjudicator."""
    return hardware_decode_verdict(m.decoder_name, m.is_platform_decoder)


def test_hardware_gate_passes_on_vaapi():
    m = MediaState()
    m.feed(_props_event("p1", kVideoDecoderName="VaapiVideoDecoder", kIsPlatformVideoDecoder="true"))
    ok, reason = _verdict(m)
    check("VaapiVideoDecoder passes the P4 gate", ok, reason)

    # Substring match: an older path stringifies as VaapiVideoDecodeAccelerator.
    m2 = MediaState()
    m2.feed(_props_event("p2", kVideoDecoderName="VaapiVideoDecodeAccelerator",
                         kIsPlatformVideoDecoder="true"))
    ok2, reason2 = _verdict(m2)
    check("VaapiVideoDecodeAccelerator also passes", ok2, reason2)


def test_hardware_gate_fails_on_software():
    # Dav1dVideoDecoder is AV1 *software* decode: if it appears, the AV1 steering broke.
    for name in ("Dav1dVideoDecoder", "VpxVideoDecoder", "FFmpegVideoDecoder"):
        m = MediaState()
        m.feed(_props_event("p1", kVideoDecoderName=name, kIsPlatformVideoDecoder="false"))
        ok, reason = _verdict(m)
        check(f"{name} FAILS the P4 gate", not ok, reason)
        check(f"{name} is named in the failure reason", name in reason, reason)


def test_hardware_gate_fails_when_nothing_played():
    # No observation must never read as a pass.
    m = MediaState()
    ok, reason = _verdict(m)
    check("no decoder observed FAILS (never a silent pass)", not ok, reason)
    check("failure explains that no video played", "no kVideoDecoderName" in reason, reason)


def test_hardware_gate_fails_when_the_platform_flag_never_arrived():
    """A decoder name with no `kIsPlatformVideoDecoder` is a BROKEN PROBE, not software decode."""
    m = MediaState()
    m.feed(_props_event("p1", kVideoDecoderName="VaapiVideoDecoder"))
    ok, reason = _verdict(m)
    check("missing platform flag FAILS", not ok, reason)
    check("and says the flag is missing, not that decode was software",
          "no kIsPlatformVideoDecoder" in reason, reason)


def test_hardware_gate_fails_on_software_decoder_claiming_to_be_platform():
    """The `"Vaapi" in name` check stands on its own: a non-VA-API platform decoder still fails."""
    m = MediaState()
    m.feed(_props_event("p1", kVideoDecoderName="Dav1dVideoDecoder", kIsPlatformVideoDecoder="true"))
    ok, reason = _verdict(m)
    check("non-Vaapi name fails even when flagged as a platform decoder", not ok, reason)
    check("the failure names the decoder", "Dav1d" in reason, reason)


def test_hardware_gate_fails_if_platform_flag_false():
    # A "Vaapi..." name alone is not enough; both conditions are required.
    m = MediaState()
    m.feed(_props_event("p1", kVideoDecoderName="VaapiVideoDecoder", kIsPlatformVideoDecoder="false"))
    ok, _ = _verdict(m)
    check("name alone is not enough; platform flag must be true", not ok)


# ---- the smoke-gate assertion machinery (T6) -------------------------------------------------

def test_run_assertions_demands_exactly_true():
    """Only exactly True passes; "false", 1 or undefined (a misspelled property) is broken."""
    for value in (True,):
        check(f"{value!r} passes", run_assertions(lambda _e: value, ["x"]) == [])
    for value in (1, "true", "false", "", 0, None, [], [1], {}):
        fails = run_assertions(lambda _e, v=value: v, ["x"])
        check(f"{value!r} FAILS (not exactly True)", fails == [("x", value)], repr(fails))


def test_run_assertions_reports_every_failure_not_just_the_first():
    """A smoke run that stops at the first red tells you one thing per CI cycle."""
    seen = []

    def ev(expr):
        seen.append(expr)
        return expr == "b"

    fails = run_assertions(ev, ["a", "b", "c"])
    check("all three evaluated", seen == ["a", "b", "c"], repr(seen))
    check("two failures collected", [e for e, _ in fails] == ["a", "c"], repr(fails))


def test_codec_assertions_cover_all_three_apis_and_a_negative_control():
    """AV1 must look unsupported through every API YouTube probes, while OK_MIMES stay supported."""
    a = codec_assertions(AV1_MIME, OK_MIMES)
    av1 = [x for x in a if "av01" in x]
    check("MediaSource.isTypeSupported probed for AV1",
          any("isTypeSupported" in x and "=== false" in x for x in av1))
    check("canPlayType probed for AV1", any("canPlayType" in x and "=== ''" in x for x in av1))
    check("mediaCapabilities.decodingInfo probed for AV1",
          any("decodingInfo" in x and "supported === false" in x for x in av1))
    for m in OK_MIMES:
        codec = m.split('codecs="')[1].rstrip('"')
        rows = [x for x in a if codec in x]
        check(f"{codec} asserted supported via MediaSource",
              any("isTypeSupported" in x and "=== true" in x for x in rows), repr(rows))
        check(f"{codec} asserted supported via canPlayType",
              any("canPlayType" in x and "!== ''" in x for x in rows), repr(rows))
    check("no AV1 assertion demands support", not any("av01" in x and "=== true" in x for x in a))


def test_codec_assertions_escape_the_mime_string():
    """The mime is interpolated into a single-quoted JS string. A quote in it would break out."""
    a = codec_assertions("video/mp4; codecs='x'", ())
    check("single quote escaped", all("codecs=\\'x\\'" in x for x in a), repr(a))


def test_av1_baseline_asserts_the_unsteered_engine_supports_av1():
    """The unsteered engine must support AV1, or every steering assertion passes vacuously."""
    b = av1_baseline_assertion(AV1_MIME)
    check("baseline demands AV1 is supported BEFORE steering", "=== true" in b and "av01" in b, b)
    check("baseline is the opposite of the post-steering assertion",
          b.replace("=== true", "=== false") in codec_assertions(AV1_MIME, ()), b)


def test_assertion_failure_is_not_retryable():
    """Exit-code taxonomy: an assertion is a product verdict (2), never a transport hiccup (4)."""
    from cdp import RETRYABLE
    check("AssertionFailure is not in RETRYABLE", not issubclass(AssertionFailure, RETRYABLE))


def test_av1_steering_script_has_exactly_one_source():
    """`config/scripts/av1_steering.js` is the only copy; launcher and `just smoke` both use it."""
    js = pathlib.Path(_ROOT, "config", "scripts", "av1_steering.js")
    check("config/scripts/av1_steering.js exists", js.is_file())
    src = js.read_text()
    for api in ("MediaSource.isTypeSupported", "canPlayType", "decodingInfo"):
        check(f"the script overrides {api}", api in src)
    check("the script cannot terminate the generated raw literal", ')JS"' not in src)

    nav = pathlib.Path(_ROOT, "launcher", "src", "navigator.cpp").read_text()
    check("navigator.cpp holds no inline copy of the script",
          "isTypeSupported" not in nav and "decodingInfo" not in nav)
    check("navigator.cpp installs it from the ScriptLibrary registry", '"av1_steering"' in nav)

    cmake = pathlib.Path(_ROOT, "launcher", "CMakeLists.txt").read_text()
    check("CMake regenerates the registry when the scripts change",
          "CMAKE_CONFIGURE_DEPENDS" in cmake and "config/scripts" in cmake)


# ---- import hygiene -------------------------------------------------------------------------

def test_importing_does_not_connect():
    """Importing cdp must not open a socket or run the CLI. The pytest harness depends on this."""
    check("CDP class is importable without connecting", callable(CDP))


if __name__ == "__main__":
    for fn in sorted(
        (v for k, v in list(globals().items()) if k.startswith("test_")),
        key=lambda f: f.__code__.co_firstlineno,
    ):
        fn()
    print()
    if _failures:
        print(f"cdp_lib: {len(_failures)} FAILED: {', '.join(_failures)}")
        sys.exit(1)
    print("cdp_lib: all assertions passed")
