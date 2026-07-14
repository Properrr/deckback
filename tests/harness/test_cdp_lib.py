#!/usr/bin/env python3
"""L0 tests for the pure parts of scripts/cdp.py — no socket, no engine, no Deck.

Covers `key_spec` (what we are allowed to synthesize), `MediaState` (the decoder-identity
accumulator, TEST-PLAN §5), and the `--assert`/`--baseline` machinery `just smoke` gates on (T6).
All are pure functions of their input, which is exactly why they were factored out of the socket
code.

`MediaState` accumulates; it does not adjudicate. The P4 verdict lives once, in
`tests/deck/lib/probes.py:hardware_decode_verdict`. The gate tests below drive the whole seam —
feed events in, read a verdict out — because the two halves living in different files is the only
reason there is a seam to get wrong.

Run: python3 tests/harness/test_cdp_lib.py   (also run by tests/harness/run.sh and CI)
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
    # Tree-verified in cobalt/src/starboard/key.h: kSbKeyMediaRewind = 0xE3, ..FastForward = 0xE4.
    check("MediaRewind is VK 227", key_spec("MediaRewind")[2] == 227)
    check("MediaFastForward is VK 228", key_spec("MediaFastForward")[2] == 228)
    # Delete(46) is the OSK erase candidate: kSbKeyBack == kSbKeyBackspace == 8, so Backspace is
    # predicted to pop the view rather than erase (input-ux §8.4).
    check("Delete is VK 46", key_spec("Delete")[2] == 46)
    check("Backspace is VK 8", key_spec("Backspace")[2] == 8)
    check("aliases resolve (Down -> ArrowDown)", key_spec("Down") == key_spec("ArrowDown"))
    # Named keys carry no `text` -> dispatched as rawKeyDown, no char event.
    check("named keys carry no text", key_spec("ArrowLeft")[3] == "")


def test_printable_keys():
    # Printable keys MUST carry `text`, or Blink never fires beforeinput/textInput and nothing lands
    # in a focused <input>. This is the whole text-entry mechanism.
    check("'a' -> KeyA/65 with text", key_spec("a") == ("a", "KeyA", 65, "a"))
    check("'N' keeps the physical code", key_spec("N") == ("N", "KeyN", 78, "N"))
    check("'7' -> Digit7/55", key_spec("7") == ("7", "Digit7", 55, "7"))
    check("space -> Space/32", key_spec(" ") == (" ", "Space", 32, " "))
    # A shifted char reports the physical key it comes from: '?' lives on Slash.
    check("'?' -> Slash/191", key_spec("?") == ("?", "Slash", 191, "?"))
    check("'/' -> Slash/191 too", key_spec("/") == ("/", "Slash", 191, "/"))
    check("'!' -> Digit1/49", key_spec("!") == ("!", "Digit1", 49, "!"))


def test_refuses_to_guess():
    # No guessing: an unknown name yields None so the caller raises, rather than dispatching a
    # plausible-looking key that silently does nothing.
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
    """The engine re-emits properties as null while tearing a player down. A null must not erase a
    decoder name we already observed, or the gate reads 'no decoder' at the end of every video."""
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

    # Substring match: an older path stringifies as VaapiVideoDecodeAccelerator (TEST-PLAN §5).
    m2 = MediaState()
    m2.feed(_props_event("p2", kVideoDecoderName="VaapiVideoDecodeAccelerator",
                         kIsPlatformVideoDecoder="true"))
    ok2, reason2 = _verdict(m2)
    check("VaapiVideoDecodeAccelerator also passes", ok2, reason2)


def test_hardware_gate_fails_on_software():
    # Dav1dVideoDecoder is AV1 *software* decode — it must never appear, and if it does the AV1
    # steering broke. This assertion is the difference between "slow" and "we shipped a regression".
    for name in ("Dav1dVideoDecoder", "VpxVideoDecoder", "FFmpegVideoDecoder"):
        m = MediaState()
        m.feed(_props_event("p1", kVideoDecoderName=name, kIsPlatformVideoDecoder="false"))
        ok, reason = _verdict(m)
        check(f"{name} FAILS the P4 gate", not ok, reason)
        check(f"{name} is named in the failure reason", name in reason, reason)


def test_hardware_gate_fails_when_nothing_played():
    # The dangerous case, and the one the power gate got wrong: no observation must never read as a
    # pass. A silent absence of evidence is not evidence of hardware decode.
    m = MediaState()
    ok, reason = _verdict(m)
    check("no decoder observed FAILS (never a silent pass)", not ok, reason)
    check("failure explains that no video played", "no kVideoDecoderName" in reason, reason)


def test_hardware_gate_fails_when_the_platform_flag_never_arrived():
    """A decoder name with no `kIsPlatformVideoDecoder` is a BROKEN PROBE, not a software decoder.

    The two must be told apart. The copy of this gate that used to live in cdp.py read a missing flag
    as `false` and reported "software decode", sending whoever read the log to debug VA-API when the
    real fault was that the Media domain had not finished reporting.
    """
    m = MediaState()
    m.feed(_props_event("p1", kVideoDecoderName="VaapiVideoDecoder"))
    ok, reason = _verdict(m)
    check("missing platform flag FAILS", not ok, reason)
    check("and says the flag is missing, not that decode was software",
          "no kIsPlatformVideoDecoder" in reason, reason)


def test_hardware_gate_fails_on_software_decoder_claiming_to_be_platform():
    """The `"Vaapi" in name` check must stand on its own.

    Found by mutation: every other software-decoder test also sets kIsPlatformVideoDecoder=false, so
    they all short-circuit on the platform flag and never reach the name check. Deleting the name
    check left the suite green. A decoder that reports itself as a platform decoder but is not VA-API
    (a v4l2/other-vendor accelerator, or a mislabeled path) must still fail the P4 gate, which is
    specifically about VA-API.
    """
    m = MediaState()
    m.feed(_props_event("p1", kVideoDecoderName="Dav1dVideoDecoder", kIsPlatformVideoDecoder="true"))
    ok, reason = _verdict(m)
    check("non-Vaapi name fails even when flagged as a platform decoder", not ok, reason)
    check("the failure names the decoder", "Dav1d" in reason, reason)


def test_hardware_gate_fails_if_platform_flag_false():
    # A decoder named "Vaapi..." but not flagged as a platform decoder must not pass on the name
    # alone — both conditions are required by TEST-PLAN §5.
    m = MediaState()
    m.feed(_props_event("p1", kVideoDecoderName="VaapiVideoDecoder", kIsPlatformVideoDecoder="false"))
    ok, _ = _verdict(m)
    check("name alone is not enough; platform flag must be true", not ok)


# ---- the smoke-gate assertion machinery (T6) -------------------------------------------------

def test_run_assertions_demands_exactly_true():
    """Truthy is not true. A predicate that returns "false", 1, or undefined is a broken predicate.

    `undefined` is the one that matters: it is what `Runtime.evaluate` returns when we misspell a
    property, and under a truthiness test it reads as a clean failure while under a `== true` test it
    reads as a clean pass. Neither is what happened. Only `is True` distinguishes them.
    """
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
    """T6: AV1 must look unsupported through every API YouTube probes, and only through those.

    The `ok_mimes` half is not decoration. Steering that reported *everything* unsupported would
    satisfy an AV1-only assertion perfectly while YouTube served nothing at all — a green gate over a
    black screen.
    """
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
    """The control for the control.

    Without this the whole T6 gate is vacuous on an engine built with no AV1 decoder: every steering
    assertion passes because there was never anything to steer. That is `just power` reporting
    `mean 0.00 W … PASS` from a battery with no telemetry, in a different costume.
    """
    b = av1_baseline_assertion(AV1_MIME)
    check("baseline demands AV1 is supported BEFORE steering", "=== true" in b and "av01" in b, b)
    check("baseline is the opposite of the post-steering assertion",
          b.replace("=== true", "=== false") in codec_assertions(AV1_MIME, ()), b)


def test_assertion_failure_is_not_retryable():
    """Exit-code taxonomy: an assertion is a product verdict (2), never a transport hiccup (4)."""
    from cdp import RETRYABLE
    check("AssertionFailure is not in RETRYABLE", not issubclass(AssertionFailure, RETRYABLE))


def test_av1_steering_script_has_exactly_one_source():
    """`config/scripts/av1_steering.js` is the only copy.

    The launcher embeds all of config/scripts/ as the ScriptLibrary registry (CMake generates
    scripts_registry.hpp) and `just smoke` injects the same file. A second copy — a C++ string
    literal, a Python constant — is a copy that goes stale silently, and the symptom is a green
    smoke run gating a script nobody ships.
    """
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
