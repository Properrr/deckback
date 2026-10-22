"""Codecs, decoder identity, display corruption and Widevine on the running app.

Page-level checks show what YouTube believes; decoder identity comes only from the CDP Media domain,
and display correctness only from pixels.
"""

from __future__ import annotations

import json
import time

import pytest

from lib import probes

PLAY_EXPR = "(function(){var v=document.querySelector('video');if(!v)return false;v.play();return true;})()"
# The video has decoded at least one frame: a size is known and the playhead moved.
DECODING_EXPR = (
    "(function(){var v=document.querySelector('video');"
    "return !!v && v.videoHeight > 0 && v.currentTime > 0;})()"
)


@pytest.mark.gate
def test_tv_user_agent_survived(leanback):
    """The page's `navigator.userAgent` is still the TV UA after Leanback's target teardown."""
    ua = leanback.evaluate(probes.UA_EXPR)
    assert ua, "navigator.userAgent is empty"
    assert "Cobalt" in ua, (
        f"the TV UA is not in effect (navigator.userAgent = {ua!r}). YouTube will serve the desktop "
        "site; every other test in this suite is measuring the wrong app."
    )


@pytest.mark.gate
def test_av1_reports_unsupported_and_vp9_does_not(leanback):
    """AV1 steering reports AV1 unsupported while VP9/H.264 stay supported (negative control)."""
    av1 = probes.parse_codec_support(leanback.evaluate(probes.codec_support_expr(probes.AV1_MIME)))
    assert av1[0] is False, f"MediaSource.isTypeSupported(av01) should be False, got {av1[0]!r}"
    assert av1[1] is False, f"canPlayType(av01) should be falsy, got {av1[1]!r}"

    for mime in (probes.VP9_MIME, probes.VP09_MIME, probes.H264_MIME):
        sup = probes.parse_codec_support(leanback.evaluate(probes.codec_support_expr(mime)))
        assert sup[0] is True, f"{mime} must stay supported, got {sup[0]!r} (steering over-reached)"
        assert sup[1] is True, f"canPlayType({mime}) must stay truthy, got {sup[1]!r}"


@pytest.mark.gate
@pytest.mark.playback
def test_hardware_decode_is_vaapi(leanback):
    """Playback uses a platform decoder whose name contains `Vaapi`; a missing value fails."""
    leanback.enable_media()
    if not leanback.evaluate(PLAY_EXPR):
        pytest.skip("no <video> element; open a video first (this test needs playback)")

    # playerPropertiesChanged arrives when the decoder is chosen, so poll.
    deadline = time.time() + 30
    while time.time() < deadline:
        leanback.pump(1.0)
        if leanback.media.decoder_name:
            break
    if not leanback.media.decoder_name and leanback.evaluate(DECODING_EXPR) is not True:
        pytest.skip("the video never decoded a frame (environment: pick a profile and play a video)")

    ok, reason = probes.hardware_decode_verdict(
        leanback.media.decoder_name, leanback.media.is_platform_decoder
    )
    assert ok, f"hardware decode gate failed: {reason}"


@pytest.mark.probe
@pytest.mark.playback
def test_no_software_decoder_ever_appears(leanback):
    """No software decoder (e.g. Dav1d, meaning AV1 slipped past the steering) is in use."""
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
    """No corrupted frames and under 2% dropped, per getVideoPlaybackQuality()."""
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
    """A screenshot during playback shows no green-band rows (the pixel gate decoder metrics miss)."""
    leanback.enable_media()
    if not leanback.evaluate(PLAY_EXPR):
        pytest.skip("no <video> element; open a video first (this test needs playback)")
    # Let 2 s of video paint before sampling.
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
    """`com.widevine.alpha` key-system access resolves; fails while no CDM is installed."""
    assert leanback.evaluate(probes.WIDEVINE_START) is True
    settled = leanback.wait_for(probes.WIDEVINE_SETTLED, timeout=15)
    result = leanback.evaluate(probes.WIDEVINE_RESULT) if settled else "timeout"
    assert result == "ok", (
        f"Widevine is not available (requestMediaKeySystemAccess -> {result!r}). Expected today: "
        "content_shell registers no CDM. This failure IS the spec for the P7 patch."
    )
