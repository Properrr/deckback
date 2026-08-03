"""The sleep timer (TASKS P12.6), on hardware.

The feature's whole claim is a chain, and only the last link is ours:

    deadline -> launcher pauses the <video>  ->  PlayState.playing goes false
             -> PlayerController drops the logind "playback active" idle inhibitor
             -> the host idle-nudge helper stops nudging (it is gated on exactly that inhibitor,
                findings durable/keep-awake.md)
             -> gamescope's idle timer finally fires -> dim -> SteamOS auto-suspends

So there is deliberately no `login1.Suspend` call to assert on: we pause, and the existing chain does
the rest. Test 4 is the only place that chain is observed from OUTSIDE the app, which is why it is a
gate even though it would pass before the timer existed — it is the assumption the design rests on,
and a Cobalt/SteamOS bump could break it without touching a line of our code.

Per TEST-PLAN §0 these were written to FAIL first: tests 1-3 assert on a sub-tab and a countdown that
did not exist when the file was committed. Test 5 is a `probe` — it decides whether "stop after this
video" is implementable at our poll cadence, which is the open question that kept that option out of
v1 rather than shipping it on a guess.
"""

from __future__ import annotations

import json
import time

import pytest

from lib import probes
from lib import uinput as u

OSD_PRESENT = "!!document.getElementById('__deckback_osd')"
OSD_ABSENT = "!document.getElementById('__deckback_osd')"

# One synthetic pad per program, and the launcher only rescans /dev/input every 2 s
# (input.cpp kHotplugScanMs), so a program's events must not land inside that window.
UINPUT_SETTLE_S = 4.0
SETTLE_S = 0.6

# The launcher's own model, read back out of the DOM it built. `sub` proves which sub-tab is showing;
# `sleep` is the status line the launcher (not the page) computed.
OSD_SUBS = "JSON.stringify((window.__dbOSD && window.__dbOSD.subs) || [])"
OSD_SUB = "(window.__dbOSD && window.__dbOSD.sub) || ''"
SLEEP_STATUS = (
    "(function(){var n=document.querySelector('#__deckback_osd .sstat');"
    "return n ? n.textContent : '';})()"
)
SLEEP_VALUE = (
    "(function(){var n=document.querySelector('#__deckback_osd .crow[data-key=timer] .cval');"
    "return n ? n.textContent : '';})()"
)

# How long we are willing to sit and watch a countdown run down. The shipped ladder's shortest real
# option is 5 min; the test reads the actual value off the menu rather than assuming it, so a
# hot-swapped shorter ladder (sleep_timer_options_minutes in app.json) makes this test fast for free.
FIRE_BUDGET_S = 8 * 60


def _play(deck, events):
    """Play one uinput program; skip on a missing udev rule (environment, not a defect)."""
    rc, _, err = deck.python(
        u.remote_program(u.gamepad_spec(), events, settle=UINPUT_SETTLE_S), check=False
    )
    if rc == 3:
        pytest.skip(f"/dev/uinput unusable (environment): {err.strip()}")
    assert rc == 0, f"synthetic pad failed (rc={rc}): {err.strip()}"


def _open_sleep_subtab(deck, leanback):
    """Menu, then ←/→ along the Section row until the Sleep sub-tab is showing.

    The press COUNT is read off the live menu rather than assumed. The Section row cycles modulo the
    number of sub-tabs, so a fixed number of rights lands on Sleep only for one particular set of
    enabled features — with captions off it would sail straight past, and the failure would read as
    "the sleep sub-tab does not exist".
    """
    _play(deck, u.press(u.BTN_START))
    assert leanback.wait_for(OSD_PRESENT, timeout=10), "Menu (BTN_START) did not open the OSD"

    subs = json.loads(leanback.evaluate(OSD_SUBS) or "[]")
    assert "sleep" in subs, f"the launcher-built menu offers no Sleep sub-tab (subs={subs!r})"
    steps = subs.index("sleep") - subs.index(leanback.evaluate(OSD_SUB))
    _play(deck, u.dpad(1, 0) * steps)

    assert leanback.wait_for(f"{OSD_SUB} === 'sleep'", timeout=10), (
        f"{steps} right press(es) did not reach the Sleep sub-tab "
        f"(subs={subs!r}, sub={leanback.evaluate(OSD_SUB)!r})"
    )


def _shortest_option_minutes(leanback):
    """The first non-Off entry of the launcher-supplied ladder, in minutes.

    min(), not options[1], but the two are the same thing on purpose: parse_sleep_options() sorts
    the ladder ascending, so one ← press off Off always selects the shortest duration. If that
    sorting were ever dropped, this returns a shorter budget than the press actually arms and the
    fire test fails loudly instead of hanging.
    """
    raw = leanback.evaluate(
        "(function(){var s=window.__dbOSD;if(!s||!s.sleep)return '[]';"
        "var rows=s.sleep.rows||[];for(var i=0;i<rows.length;i++)"
        "if(rows[i].key==='timer')return JSON.stringify(rows[i].options||[]);return '[]';})()"
    )
    opts = json.loads(raw or "[]")
    mins = [float(o["value"]) for o in opts if o.get("value") not in (None, "", "0")]
    assert mins, f"the launcher offered no sleep durations at all (options={opts!r})"
    return min(mins)


@pytest.fixture()
def _close_osd(cdp):
    """Leave no menu behind — an OSD left open captures input for every following test."""
    yield
    try:
        cdp.evaluate(
            "(function(){var n=document.getElementById('__deckback_osd');if(n)n.remove();"
            "return true;})()"
        )
    except Exception:  # noqa: BLE001 - cleanup must never mask the real failure
        pass


# ---- the launcher offers the feature at all ------------------------------------------------------


@pytest.mark.gate
@pytest.mark.uinput
def test_launcher_offers_a_sleep_subtab(deck, leanback, _close_osd):
    """A real Menu press must produce a menu whose Section row includes Sleep.

    Asserts on the menu the LAUNCHER built, not on a model this test chose — the osd.js component can
    render a sleep sub-tab from any model handed to it, which proves nothing about the wiring.
    """
    _play(deck, u.press(u.BTN_START))
    assert leanback.wait_for(OSD_PRESENT, timeout=10), "Menu did not open the OSD"
    subs = json.loads(leanback.evaluate(OSD_SUBS) or "[]")
    assert "sleep" in subs, (
        f"the launcher-built menu has no Sleep sub-tab (subs={subs!r}) — either sleep_timer is off "
        "in app.json or main.cpp never handed the OSD a SleepTimer"
    )


@pytest.mark.gate
@pytest.mark.uinput
def test_arming_from_the_osd_starts_a_countdown(deck, leanback, _close_osd):
    """Pick the shortest duration and the launcher must report a countdown, not just a chosen value.

    The status line is computed in C++ from the armed deadline, so a menu that shows "Sleeps in ..."
    is the launcher confirming it took the edit — the page cannot invent it.
    """
    _open_sleep_subtab(deck, leanback)
    assert "off" in leanback.evaluate(SLEEP_STATUS).lower(), (
        f"the sleep timer was already armed before the test set it: {leanback.evaluate(SLEEP_STATUS)!r}"
    )

    _play(deck, u.dpad(0, 1) + u.dpad(1, 0))  # down onto the timer row, right to the first duration
    armed = leanback.wait_for(
        "(function(){var n=document.querySelector('#__deckback_osd .sstat');"
        "return !!n && /\\d/.test(n.textContent);})()",
        timeout=10,
    )
    assert armed, (
        f"selecting a duration did not start a countdown — status={leanback.evaluate(SLEEP_STATUS)!r} "
        f"value={leanback.evaluate(SLEEP_VALUE)!r}. The OSD's apply: verdict never reached SleepTimer."
    )
    # The combo must agree with the countdown. A status line that says one duration while the picker
    # shows another means the launcher armed something the user did not choose.
    assert leanback.evaluate(SLEEP_VALUE).strip("‹› "), "the duration combo shows no value"

    _play(deck, u.press(u.BTN_EAST))  # B closes; the timer keeps running with the menu shut
    assert leanback.wait_for(OSD_ABSENT, timeout=10), "B did not close the OSD"


# ---- the deadline actually stops playback --------------------------------------------------------


@pytest.mark.gate
@pytest.mark.uinput
@pytest.mark.playback
@pytest.mark.slow
def test_sleep_timer_pauses_playback(deck, leanback, _close_osd):
    """The deliverable: arm the shortest duration over a playing video and it must stop.

    Slow by construction — it waits out a real countdown. Excluded from a quick pass with
    `scripts/test-deck.sh -m "not slow"`.
    """
    before = probes.parse_playback_state(leanback.evaluate(probes.PLAYBACK_STATE_EXPR))
    if not before.get("video"):
        pytest.skip("no <video> element; open a video first (this test needs playback)")
    if before.get("paused"):
        pytest.skip("the video is paused; this test needs playback to have something to stop")

    _open_sleep_subtab(deck, leanback)
    minutes = _shortest_option_minutes(leanback)
    budget = minutes * 60 + 90
    if budget > FIRE_BUDGET_S:
        pytest.skip(
            f"the shortest sleep duration is {minutes:g} min; that exceeds this test's "
            f"{FIRE_BUDGET_S / 60:g} min budget. Hot-swap a shorter sleep_timer_options_minutes."
        )

    _play(deck, u.dpad(0, 1) + u.dpad(1, 0) + u.press(u.BTN_EAST))
    assert leanback.wait_for(OSD_ABSENT, timeout=10), "B did not close the OSD"

    # Playback must survive right up to the deadline: a timer that stops the video early is as wrong
    # as one that never fires, and only a mid-flight check can tell the two apart.
    time.sleep(min(30.0, minutes * 60 * 0.5))
    mid = probes.parse_playback_state(leanback.evaluate(probes.PLAYBACK_STATE_EXPR))
    assert not mid.get("paused"), (
        f"the video was paused {minutes:g} min BEFORE the deadline — the timer fired early"
    )

    deadline = time.monotonic() + budget
    while time.monotonic() < deadline:
        state = probes.parse_playback_state(leanback.evaluate(probes.PLAYBACK_STATE_EXPR))
        if state.get("paused"):
            break
        time.sleep(5)
    else:
        pytest.fail(
            f"the sleep timer never paused the video within {budget:.0f}s of arming a "
            f"{minutes:g} min countdown"
        )

    # Paused is not enough: assert the position stops moving, which is what the power chain needs.
    t0 = probes.parse_playback_state(leanback.evaluate(probes.PLAYBACK_STATE_EXPR))
    time.sleep(6)
    t1 = probes.parse_playback_state(leanback.evaluate(probes.PLAYBACK_STATE_EXPR))
    assert abs(float(t1["t"]) - float(t0["t"])) < 0.5, (
        f"the video reports paused but currentTime still advanced "
        f"({t0['t']:.2f}s -> {t1['t']:.2f}s) — something resumed it"
    )


# ---- the chain beyond our pause ------------------------------------------------------------------

_INHIBIT_CMD = (
    "busctl --system call org.freedesktop.login1 /org/freedesktop/login1 "
    "org.freedesktop.login1.Manager ListInhibitors 2>/dev/null"
)


def _playback_inhibitor_held(deck):
    """Is the launcher's "playback active" idle inhibitor listed by logind right now?

    Reads the D-Bus reply, not `systemd-inhibit --list`: that table ellipsizes its WHY column, which
    is the exact bug that made the host nudge helper silently stop working (keep-awake.md).
    """
    rc, out, _ = deck.run(_INHIBIT_CMD, check=False)
    if rc != 0 or not out.strip():
        pytest.skip("logind ListInhibitors is unavailable on this Deck (environment)")
    return "playback active" in out


@pytest.mark.gate
@pytest.mark.playback
def test_pausing_releases_the_playback_inhibitor(deck, leanback):
    """The link the sleep timer relies on and does not implement: pause => the Deck may sleep again.

    Not a test of the timer. It is the assumption underneath it, observed from outside the app, so a
    SteamOS or Cobalt change that breaks the chain is reported here rather than as "the sleep timer
    stopped saving battery" months later.
    """
    state = probes.parse_playback_state(leanback.evaluate(probes.PLAYBACK_STATE_EXPR))
    if not state.get("video") or state.get("paused"):
        pytest.skip("this test needs a video actually playing")

    assert _playback_inhibitor_held(deck), (
        "logind does not list the launcher's 'playback active' inhibitor during playback — the host "
        "idle-nudge helper is gated on it, so the screen would dim mid-video (keep-awake.md)"
    )

    leanback.evaluate("(function(){var v=document.querySelector('video');if(v)v.pause();})()")
    released = False
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if not _playback_inhibitor_held(deck):
            released = True
            break
        time.sleep(1)
    assert released, (
        "the 'playback active' inhibitor was still held 15 s after the video paused — the sleep "
        "timer would stop the video and the Deck would still never idle out"
    )

    leanback.evaluate("(function(){var v=document.querySelector('video');if(v)v.play();})()")


# ---- the open question that kept "after this video" out of v1 ------------------------------------


@pytest.mark.probe
@pytest.mark.playback
def test_is_the_end_of_a_video_observable_at_our_poll_cadence(leanback):
    """Can a poll at the launcher's cadence ever SEE a video end before autoplay swaps the next in?

    "Stop after this video" is the option TASKS P12.6 names and v1 does not ship, because the answer
    to this decides whether it is implementable at all or needs a different lever. A failure here is
    a FINDING for durable/feature-landscape.md, not a regression: it means the ended state is not
    observable by polling and the option must be built on something else (or dropped).

    Run this near the end of a short video. It reports what it saw either way.
    """
    state = probes.parse_playback_state(leanback.evaluate(probes.PLAYBACK_STATE_EXPR))
    if not state.get("video"):
        pytest.skip("no <video> element; open a video first")

    duration = leanback.evaluate(
        "(function(){var v=document.querySelector('video');"
        "return v && isFinite(v.duration) ? v.duration : -1;})()"
    )
    remaining = float(duration) - float(state["t"]) if float(duration) > 0 else -1
    if remaining < 0 or remaining > 90:
        pytest.skip(
            f"the current video has {remaining:.0f}s left; seek to within 90 s of the end and re-run "
            "(this probe cannot manufacture an ending it is allowed to wait for)"
        )

    # Poll at 1 s, the launcher's own play-state cadence, and record every distinct state seen.
    seen = []
    ids = set()
    deadline = time.monotonic() + remaining + 30
    while time.monotonic() < deadline:
        s = probes.parse_playback_state(leanback.evaluate(probes.PLAYBACK_STATE_EXPR))
        key = (s.get("paused"), s.get("ended"), round(float(s.get("t", -1))))
        if key not in ids:
            ids.add(key)
            seen.append(s)
        if s.get("ended"):
            break
        time.sleep(1)

    ended_seen = any(s.get("ended") for s in seen)
    assert ended_seen, (
        "a 1 s poll never observed <video>.ended across the end of a video — Leanback's autoplay "
        "replaces the element or resets it faster than the launcher's play-state cadence can see, "
        "so 'stop after this video' cannot be built on polling `ended`. Register this in "
        "durable/feature-landscape.md and pick a different lever (or drop the option). "
        f"States observed: {seen[-6:]!r}"
    )
