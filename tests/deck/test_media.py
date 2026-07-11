"""Codecs, decoder identity, and the AV1 dispute (TEST-PLAN §5).

Two channels that are constantly confused:

  `Runtime.evaluate` tells you what **YouTube believes** about us. It guards R1 (the UA bet) and
  proves the steering script ran. It knows nothing about the GPU.

  The CDP **Media domain** tells you what the **engine actually did**. `kVideoDecoderName` and
  `kIsPlatformVideoDecoder` are the only supported way to learn the decoder identity, and together
  they are the P4 hardware-decode gate.

`video.getVideoPlaybackQuality()` gives frame counts and *no decoder name*. Asserting on dropped
frames alone would pass happily on a software decoder that happens to keep up at 720p.

This file also contains the only honest test of the AV1 dispute
(`.internal/findings/durable/hardware.md`): libva on the OLED unit advertises `AV1Profile0`, but we
have never seen AV1 decoded *through* `VaapiVideoDecoder`. Assert on the decoder name, not on what
libva claims it can do.
"""

from __future__ import annotations

import json
import time

import pytest

from lib import probes

PLAY_EXPR = "(function(){var v=document.querySelector('video');if(!v)return false;v.play();return true;})()"


@pytest.mark.gate
def test_tv_user_agent_survived(leanback):
    """R1 canary. `Network.setUserAgentOverride` must still be armed after Leanback's target teardown.

    Asserts on the *page's* view (`navigator.userAgent`), never on the string we handed to CDP: the
    two differ precisely when the override was lost, which is the failure worth catching.
    """
    ua = leanback.evaluate(probes.UA_EXPR)
    assert ua, "navigator.userAgent is empty"
    assert "Cobalt" in ua, (
        f"the TV UA is not in effect (navigator.userAgent = {ua!r}). YouTube will serve the desktop "
        "site; every other test in this suite is measuring the wrong app."
    )


@pytest.mark.gate
def test_av1_reports_unsupported_and_vp9_does_not(leanback):
    """The steering script (navigator.cpp) must make AV1 look unsupported and touch nothing else.

    A negative control is mandatory here: if the script were broken in the direction of reporting
    *everything* unsupported, an AV1-only assertion would still pass while YouTube served nothing.
    """
    av1 = probes.parse_codec_support(leanback.evaluate(probes.codec_support_expr(probes.AV1_MIME)))
    assert av1[0] is False, f"MediaSource.isTypeSupported(av01) should be False, got {av1[0]!r}"
    assert av1[1] is False, f"canPlayType(av01) should be falsy, got {av1[1]!r}"

    for mime in (probes.VP9_MIME, probes.VP09_MIME, probes.H264_MIME):
        sup = probes.parse_codec_support(leanback.evaluate(probes.codec_support_expr(mime)))
        assert sup[0] is True, f"{mime} must stay supported, got {sup[0]!r} (steering over-reached)"
        assert sup[1] is True, f"canPlayType({mime}) must stay truthy, got {sup[1]!r}"


@pytest.mark.gate
@pytest.mark.playback
@pytest.mark.xfail(
    reason="P4 HW decode is DISABLED: VA-API engages but ANGLE renders green-band corruption on this "
    "radeonsi stack, so we ship software decode (durable/hardware.md, m114.md). This gate stays here "
    "as the P4 goal; if it ever XPASSes, HW decode was re-enabled and the xfail must be removed — but "
    "ONLY alongside test_video_is_not_corrupt still passing (decoder identity is not display correctness).",
    strict=False,
)
def test_hardware_decode_is_vaapi(leanback):
    """The P4 goal. Pass iff `kIsPlatformVideoDecoder` and the name contains `Vaapi`.

    Both conditions, because neither alone is sufficient: `kIsPlatformVideoDecoder` has been observed
    true for decoders that are not VA-API, and a name match alone does not prove the platform path
    was taken. A *missing* value is a failure, not a pass — it means the Media domain never reported,
    which is a broken probe, and a broken probe must never look like working hardware.
    """
    leanback.enable_media()
    if not leanback.evaluate(PLAY_EXPR):
        pytest.skip("no <video> element; open a video first (this test needs playback)")

    # Poll rather than sleep: playerPropertiesChanged arrives when the decoder is chosen, not on a
    # schedule we control.
    deadline = time.time() + 30
    while time.time() < deadline:
        leanback.pump(1.0)
        if leanback.media.decoder_name:
            break

    ok, reason = probes.hardware_decode_verdict(
        leanback.media.decoder_name, leanback.media.is_platform_decoder
    )
    assert ok, f"hardware decode gate failed: {reason}"


@pytest.mark.probe
@pytest.mark.playback
def test_no_software_decoder_ever_appears(leanback):
    """`Dav1dVideoDecoder` means AV1 slipped past the steering and is burning the CPU."""
    leanback.enable_media()
    leanback.pump(5.0)
    name = leanback.media.decoder_name
    if not name:
        pytest.skip("Media domain reported no decoder (is anything playing?)")
    for sw in probes.SOFTWARE_DECODERS:
        assert sw not in name, (
            f"software decoder {name!r} in use. If this is Dav1d, the AV1 steering failed and the "
            "battery gate will fail with it."
        )


@pytest.mark.probe
@pytest.mark.playback
def test_frames_are_not_being_dropped(leanback):
    """Frame counts, which say nothing about *which* decoder — that is the test above."""
    raw = leanback.evaluate(probes.PLAYBACK_QUALITY_EXPR)
    if not raw:
        pytest.skip("no <video> with getVideoPlaybackQuality")
    total, dropped, corrupted = (int(x) for x in raw.split(","))
    if total < 60:
        pytest.skip(f"only {total} frames decoded so far; let playback run longer")
    assert corrupted == 0, f"{corrupted} corrupted frames"
    assert dropped / total < 0.02, f"{dropped}/{total} frames dropped ({dropped / total:.1%})"


@pytest.mark.gate
@pytest.mark.playback
def test_video_is_not_corrupt(leanback):
    """The PIXEL gate — the one check that would have caught the 2026-07-10 green-band regression.

    Every metric in this file is blind to display-layer corruption: on 2026-07-10 VA-API decode
    engaged, `corruptedVideoFrames` stayed 0, and the panel was full of neon-green bands anyway,
    because the fault was in ANGLE's *import* of a correctly-decoded surface (durable/hardware.md).
    So this gate reads actual pixels: capture a frame during playback and fail if a meaningful
    fraction of rows are near-pure-green bands. This is the check that CAN fail — and did, on the
    config that shipped for part of a day.
    """
    leanback.enable_media()
    if not leanback.evaluate(PLAY_EXPR):
        pytest.skip("no <video> element; open a video first (this test needs playback)")
    # Let a few frames actually paint before sampling.
    deadline = time.time() + 10
    while time.time() < deadline and (leanback.evaluate(probes.VIDEO_TIME_EXPR) or 0) < 2:
        time.sleep(0.5)
    png = leanback.screenshot_png()
    if not png:
        pytest.skip("Page.captureScreenshot returned nothing (headless paint stall?)")
    ok, reason, _ratio = probes.video_corruption_verdict(png)
    assert ok, f"video corruption gate failed: {reason}"


@pytest.mark.probe
def test_widevine_state(leanback):
    """P7. Expected to be REJECTED today.

    The shell has no `AddContentDecryptionModules` override, so `enable_widevine` alone registers
    nothing (m114.md, "Widevine registration gap"). **This test failing is the specification for the
    first `patches/` entry.** It is a probe so it reports the truth rather than blocking the build.
    """
    assert leanback.evaluate(probes.WIDEVINE_START) is True
    settled = leanback.wait_for(probes.WIDEVINE_SETTLED, timeout=15)
    result = leanback.evaluate(probes.WIDEVINE_RESULT) if settled else "timeout"
    assert result == "ok", (
        f"Widevine is not available (requestMediaKeySystemAccess -> {result!r}). Expected today: "
        "content_shell registers no CDM. This failure IS the spec for the P7 patch."
    )
