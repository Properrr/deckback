"""Capability + telemetry probes (TEST-PLAN §5).

Two channels answer different questions and are constantly confused:

* **Page-level** (`Runtime.evaluate`) — what YouTube *believes* about us. This is what guards R1 (the
  UA bet) and tells us whether the AV1 steering script took effect. It knows nothing about the GPU:
  `video.getVideoPlaybackQuality()` returns frame counts and no decoder name.

* **CDP `Media` domain** — what the *engine actually did*. `kVideoDecoderName` and
  `kIsPlatformVideoDecoder` are the only supported way to learn the decoder identity, and they are
  the P4 hardware-decode gate. (`scripts/cdp.py:MediaState` collects them.)

The pure parsers here are unit-tested on the workstation by `tests/harness/test_deck_lib.py`. That
matters more than it sounds: `parse_power_now()` replaces a shell one-liner that once reported
`mean 0.00 W … PASS` on a Deck whose battery exposes no telemetry — a perfect score from zero data.
"""

from __future__ import annotations

# ---- page-level probe expressions ----------------------------------------------------------------

# Never assert on a value we chose (TEST-PLAN §0). `navigator.userAgent` is the page's view of the
# UA, not the string we handed to Network.setUserAgentOverride — the two differ if the override was
# lost across Leanback's target teardown, which is exactly the failure worth catching.
UA_EXPR = "navigator.userAgent"

ON_LEANBACK_EXPR = "location.href.indexOf('youtube.com/tv') >= 0"

# `player_open` keys off the #/watch hash route, whose shape is verified (deep-link test 2026-07-08).
PLAYER_OPEN_EXPR = "!!document.querySelector('video') && location.hash.indexOf('/watch') >= 0"

ACTIVE_ELEMENT_EXPR = (
    "(function(){var e=document.activeElement;if(!e)return '';"
    "return (e.tagName||'')+'#'+(e.id||'')+'.'+(e.className||'')"
    "+'@'+(e.getAttribute&&e.getAttribute('aria-label')||'');})()"
)

VIDEO_TIME_EXPR = "(function(){var v=document.querySelector('video');return v?v.currentTime:-1;})()"

PLAYBACK_QUALITY_EXPR = (
    "(function(){var v=document.querySelector('video');"
    "if(!v||!v.getVideoPlaybackQuality)return '';"
    "var q=v.getVideoPlaybackQuality();"
    "return q.totalVideoFrames+','+q.droppedVideoFrames+','+q.corruptedVideoFrames;})()"
)

# The three APIs the AV1 steering script overrides. Answers "TTT"/"FFF"-style triples so one round
# trip covers all of them and they cannot disagree between polls.
def codec_support_expr(mime):
    q = mime.replace("\\", "\\\\").replace("'", "\\'")
    return (
        "(function(){var m='%s';"
        "var a=(window.MediaSource&&MediaSource.isTypeSupported)?MediaSource.isTypeSupported(m):null;"
        "var v=document.createElement('video');"
        "var b=v.canPlayType?v.canPlayType(m):null;"
        "return JSON.stringify({mse:a,canPlay:b});})()" % q
    )


AV1_MIME = 'video/mp4; codecs="av01.0.05M.08"'
VP9_MIME = 'video/webm; codecs="vp9"'
VP09_MIME = 'video/mp4; codecs="vp09.00.10.08"'
H264_MIME = 'video/mp4; codecs="avc1.640028"'

# requestMediaKeySystemAccess is async, so it cannot be read from a single Runtime.evaluate.
# Kick it off into a global, then poll. `WIDEVINE_RESULT` is 'pending' until it settles.
WIDEVINE_START = (
    "(function(){window.__dbWidevine='pending';"
    "if(!navigator.requestMediaKeySystemAccess){window.__dbWidevine='no-api';return true;}"
    "navigator.requestMediaKeySystemAccess('com.widevine.alpha',[{"
    "initDataTypes:['cenc'],"
    "videoCapabilities:[{contentType:'video/mp4; codecs=\"avc1.640028\"'}]}])"
    ".then(function(){window.__dbWidevine='ok';},"
    "function(e){window.__dbWidevine='rejected:'+(e&&e.name||'?');});"
    "return true;})()"
)
WIDEVINE_SETTLED = "window.__dbWidevine !== 'pending'"
WIDEVINE_RESULT = "window.__dbWidevine"


def parse_codec_support(raw):
    """`{"mse": bool|None, "canPlay": str|None}` -> (mse_supported, can_play_nonempty)."""
    import json

    if not raw:
        return (None, None)
    d = json.loads(raw)
    mse = d.get("mse")
    cp = d.get("canPlay")
    return (mse, bool(cp) if cp is not None else None)


# ---- battery telemetry ---------------------------------------------------------------------------

# Read from sysfs. Two units exist in the wild and must not be averaged together:
#   power_now  — microwatts, directly usable
#   current_now + voltage_now — microamps x microvolts, the fallback on packs with no power_now
POWER_NOW_PATHS = (
    "/sys/class/power_supply/BAT1/power_now",
    "/sys/class/power_supply/BAT0/power_now",
)
CURRENT_NOW_PATHS = (
    "/sys/class/power_supply/BAT1/current_now",
    "/sys/class/power_supply/BAT0/current_now",
)
VOLTAGE_NOW_PATHS = (
    "/sys/class/power_supply/BAT1/voltage_now",
    "/sys/class/power_supply/BAT0/voltage_now",
)


class NoTelemetry(RuntimeError):
    """The battery reported nothing. NOT a power-budget failure — the measurement never happened.

    This exception exists because the old `just power` averaged an empty field to 0.00 W and printed
    PASS against a "<= 9 W" budget. A gate that scores a perfect result from zero samples is not a
    gate. Callers must map this to exit 3 (ENV), never exit 2 (ASSERT).
    """


def parse_power_now(text):
    """One microwatt sample -> watts. Raises NoTelemetry on anything that is not a number.

    Rejects the empty string, whitespace, `-`, and non-numeric junk. A zero reading is *valid* (the
    Deck can genuinely be on mains) and must be returned, not swallowed — so "0" is a number and ""
    is not, and the difference is the entire point.
    """
    if text is None:
        raise NoTelemetry("no power_now reading")
    s = text.strip()
    if not s:
        raise NoTelemetry("empty power_now reading")
    try:
        uw = int(s)
    except ValueError:
        raise NoTelemetry(f"non-numeric power_now reading: {s!r}") from None
    if uw < 0:
        raise NoTelemetry(f"negative power_now reading: {uw}")
    return uw / 1_000_000.0


def parse_current_voltage(current_text, voltage_text):
    """microamps x microvolts -> watts, for packs with no `power_now`."""
    try:
        ua = int((current_text or "").strip())
        uv = int((voltage_text or "").strip())
    except ValueError:
        raise NoTelemetry("non-numeric current_now/voltage_now") from None
    if ua < 0 or uv <= 0:
        raise NoTelemetry(f"implausible current/voltage: {ua} uA, {uv} uV")
    return (ua / 1_000_000.0) * (uv / 1_000_000.0)


def summarize_power(samples, budget_w):
    """(mean, peak, verdict) over watt samples.

    Raises NoTelemetry on an empty list. An average over nothing is 0.0, which would pass any budget
    — the exact bug this module was written to make impossible to reintroduce.
    """
    if not samples:
        raise NoTelemetry("no power samples collected")
    mean = sum(samples) / len(samples)
    peak = max(samples)
    return mean, peak, ("PASS" if mean <= budget_w else "FAIL")


# ---- decoder identity (the P4 gate) --------------------------------------------------------------

# Software decoders that must never appear on a Deck. Dav1d is the AV1 software path: if it shows up,
# either the steering failed or AV1 is being served anyway (findings durable/hardware.md).
SOFTWARE_DECODERS = ("Dav1dVideoDecoder", "VpxVideoDecoder", "FFmpegVideoDecoder")


def hardware_decode_verdict(decoder_name, is_platform):
    """(ok, reason). Mirrors `scripts/cdp.py:MediaState.hardware_decode_ok`.

    Both conditions are required, and neither is redundant: `kIsPlatformVideoDecoder` alone has been
    seen true for decoders that are not VA-API, and a name match alone does not prove the platform
    path was taken. A missing value is *not* a pass — it means the Media domain never reported, which
    is a broken probe, not working hardware.
    """
    if not decoder_name:
        return (False, "no kVideoDecoderName reported (Media domain never fired?)")
    if is_platform is None:
        return (False, "no kIsPlatformVideoDecoder reported")
    for sw in SOFTWARE_DECODERS:
        if sw in decoder_name:
            return (False, f"software decoder in use: {decoder_name}")
    if not is_platform:
        return (False, f"{decoder_name} is not a platform decoder")
    if "Vaapi" not in decoder_name:
        return (False, f"platform decoder is not VA-API: {decoder_name}")
    return (True, decoder_name)


# ---- display-layer corruption (the pixel gate) ---------------------------------------------------
#
# WHY THIS EXISTS: on 2026-07-10, VA-API hardware decode (VaapiVideoDecoder) engaged correctly and
# ANGLE painted green-band corruption over every frame — yet kVideoDecoderName, kIsPlatformVideoDecoder,
# corruptedVideoFrames=0 and droppedVideoFrames=0 ALL reported PASS while the panel was striped
# (findings durable/hardware.md, milestones/m114.md). corruptedVideoFrames counts *decoder*-reported
# corruption; the fault was in ANGLE's *import* of a correctly-decoded surface, invisible to the media
# pipeline. So decoder-identity checks cannot gate display correctness — only pixels can. This is the
# check that would have caught it (F1: a check that cannot fail is not a check).

# The corruption is bright, near-pure green (~#00FF00): G high, R and B both low. Natural foliage green
# in real video is never this pure (grass/leaves carry substantial R and B). Measured separation on the
# 2026-07-10 frames was decisive: corrupt ≈0.17 band-row ratio, clean = 0.000.
GREEN_BAND_G_MIN = 200      # a corrupt pixel's green channel is at least this
GREEN_BAND_RB_MAX = 110     # ...and its red AND blue are both at most this
GREEN_BAND_ROW_FRAC = 0.40  # a "band row" is >=40% pure-green pixels across its width
GREEN_BAND_RATIO_MAX = 0.02 # fail if >2% of rows are band rows (clean frames are 0.000)


def green_band_ratio(png_bytes):
    """Fraction of image rows that are dominated by near-pure-green pixels.

    Detects the VA-API/ANGLE import corruption (horizontal neon-green banding) from a PNG screenshot.
    Pure function over bytes so it is L0-testable with synthetic images. Samples every 4th column for
    speed; the bands span the full width, so subsampling does not miss them.
    """
    import io
    from PIL import Image

    im = Image.open(io.BytesIO(png_bytes)).convert("RGB")
    w, h = im.size
    if w == 0 or h == 0:
        return 0.0
    px = im.load()
    step = 4
    cols = len(range(0, w, step))
    band_rows = 0
    for y in range(h):
        green = 0
        for x in range(0, w, step):
            r, g, b = px[x, y]
            if g >= GREEN_BAND_G_MIN and r <= GREEN_BAND_RB_MAX and b <= GREEN_BAND_RB_MAX:
                green += 1
        if green / cols >= GREEN_BAND_ROW_FRAC:
            band_rows += 1
    return band_rows / h


def video_corruption_verdict(png_bytes):
    """(ok, reason, ratio). ok iff the frame's green-band ratio is under the fail threshold."""
    ratio = green_band_ratio(png_bytes)
    if ratio > GREEN_BAND_RATIO_MAX:
        return (False, f"green-band corruption: {ratio:.1%} of rows are neon-green bands "
                       f"(VA-API/ANGLE import bug — see durable/hardware.md)", ratio)
    return (True, f"clean ({ratio:.1%} green-band rows)", ratio)


# ---- playback liveness (the T5 gate) -------------------------------------------------------------

# `currentTime` alone cannot tell a paused video from a stalled one, and the two have opposite
# owners: a paused video is the operator forgetting to press play (ENV, 3), a stalled one is the
# product (ASSERT, 2). Read both in one round trip so they cannot disagree between polls.
PLAYBACK_STATE_EXPR = (
    "(function(){var v=document.querySelector('video');"
    "if(!v)return JSON.stringify({video:false});"
    "return JSON.stringify({video:true,t:v.currentTime,paused:!!v.paused,"
    "ended:!!v.ended,ready:v.readyState});})()"
)


def parse_playback_state(raw):
    """`{'video':bool,'t':float,'paused':bool,'ended':bool,'ready':int}` or `{'video': False}`."""
    import json

    if not raw:
        raise NoTelemetry("no reply from the playback-state probe")
    return json.loads(raw)


def playback_advanced(before, after, elapsed_s, min_fraction=0.5):
    """Did the video actually play across a window of `elapsed_s` wall seconds? -> (ok, reason).

    This is the check that makes `just power` mean anything. A 9 W budget measured over a **paused**
    video passes effortlessly and describes nothing; the same shape as averaging an empty battery
    field to 0.00 W (finding F1). The number is only a playback measurement if playback happened.

    `min_fraction` of wall time, not all of it: a resume, a rebuffer, or an ad break legitimately
    costs a second or two. Zero advance is never legitimate.

    Backwards movement is its own verdict. Across a suspend/resume (P6) it means the player restarted
    the video rather than continuing it, which is exactly the "position correct" clause of the gate —
    and reporting it as "did not advance" would send the reader looking for a stall that never
    happened.
    """
    if not before.get("video") or not after.get("video"):
        return (False, "no <video> element (is a video open?)")
    if after.get("paused"):
        return (False, "the video is paused")
    if after.get("ended"):
        return (False, "the video ended during the window")
    t0, t1 = float(before["t"]), float(after["t"])
    if t1 < t0:
        return (False, f"currentTime went BACKWARDS: {t0:.2f}s -> {t1:.2f}s (the player restarted)")
    delta = t1 - t0
    need = elapsed_s * min_fraction
    if delta < need:
        return (False,
                f"currentTime advanced {delta:.2f}s over {elapsed_s:.1f}s of wall clock "
                f"(needed >= {need:.2f}s) — playback is stalled")
    return (True, f"currentTime advanced {delta:.2f}s over {elapsed_s:.1f}s")


# ---- EPP (a defect we can measure and cannot fix) ------------------------------------------------

# SteamOS resets EPP on cores 1-7 from `balance_performance` to `performance` after a resume
# (steamos#2383), costing draw and heat. Writing it back needs root, so it is infeasible from the
# Flatpak. `just soak` therefore MEASURES and REPORTS it and must never fail on it: a gate we cannot
# pass and cannot fix is a gate that gets disabled, and then it stops reporting too.
EPP_GLOB = "/sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference"
EPP_READ_CMD = f"grep -H . {EPP_GLOB} 2>/dev/null"


def parse_epp(text):
    """`grep -H . <glob>` output -> {cpu_index: preference}. Raises NoTelemetry on nothing at all."""
    out = {}
    for line in (text or "").splitlines():
        path, _, value = line.partition(":")
        value = value.strip()
        if not value:
            continue
        # .../cpu<N>/cpufreq/...
        for part in path.split("/"):
            if part.startswith("cpu") and part[3:].isdigit():
                out[int(part[3:])] = value
                break
    if not out:
        raise NoTelemetry("no energy_performance_preference nodes readable")
    return out


def epp_report(before, after):
    """(changed, summary) where `changed` maps cpu -> (before, after) for cores that moved.

    A core present in only one reading is reported as changed, with `None` on the missing side.
    Silently intersecting the two would hide a core that vanished, and "fewer cores than before" is
    not a thing we should learn about by not learning about it.
    """
    changed = {}
    for cpu in sorted(set(before) | set(after)):
        b, a = before.get(cpu), after.get(cpu)
        if b != a:
            changed[cpu] = (b, a)
    if not changed:
        return ({}, f"EPP unchanged across resume on all {len(after)} cores")
    moves = sorted({f"{b}->{a}" for b, a in changed.values()})
    return (changed,
            f"EPP changed on {len(changed)}/{len(set(before) | set(after))} cores "
            f"({', '.join(moves)}) — steamos#2383, root-only, we can only report it")


def trusted_html_js(raw_html_js_literal):
    """JS that assigns `raw_html_js_literal` (a JS string expression) through a Trusted Types policy.

    This MUST mirror `js_trusted_html` in launcher/src/overlay.cpp. It exists because youtube.com/tv
    enforces `require-trusted-types-for 'script'`, so a bare `el.innerHTML = "<h2>..."` throws and
    renders nothing. The launcher's real injection goes through the policy; a test that injects with
    a bare innerHTML is testing a technique the product does not use, and fails on the real page while
    the product works (which is exactly what happened on 2026-07-10 -- the test's inline copy drifted
    from the launcher after the launcher was fixed).
    """
    return (
        "(function(_h){try{var T=window.trustedTypes;if(!T)return _h;"
        "if(!window.__dbTTP)window.__dbTTP=T.createPolicy('deckback',"
        "{createHTML:function(x){return x;}});"
        "return window.__dbTTP.createHTML(_h);}catch(e){return _h;}})(" + raw_html_js_literal + ")"
    )
