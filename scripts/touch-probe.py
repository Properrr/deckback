#!/usr/bin/env python3
"""Does gamescope's touch PASSTHROUGH mode deliver real touch events to our page?

Runs on the WORKSTATION, drives the Deck over SSH + CDP, and needs **a human with a finger on the
panel**. It answers one question with hardware, because nothing above the compositor can be inferred
from source alone.

Why it exists. `durable/touch-lock.md` proved the app cannot take the panel from gamescope, and
`disable_touch` works around that by pinning gamescope's touch mode to **hover (0)** — the one mode
that emits nothing but pointer motion. But gamescope's `wlserver.cpp` shows mode **4 (passthrough)**
is categorically different: it calls `wlr_seat_touch_notify_down/motion(..., touch_id, ...)`, i.e.
real `wl_touch` with a per-finger id and no pointer emulation at all. Modes 1-3 only synthesize a
mouse button. So multitouch may already be reachable and we are discarding it by choice.

The unverified link is everything above gamescope: gamescope -> Xwayland -> XI2 -> Chromium's X11
ozone -> Blink. Chromium's half is present in the pinned tree (`TouchFactory::UpdateDeviceList`
accepts an XI2 device only if it carries a touch class in `Direct` mode; `--touch-devices` force-marks
one when detection fails), but whether gamescope's Xwayland advertises such a device is not knowable
from here.

What it reports:

  * whether Chromium itself thinks a touchscreen exists (`navigator.maxTouchPoints`, `ontouchstart`,
    `pointer: coarse`) — the single most informative line, and it needs no `xinput` on the Deck;
  * per touch mode, what a real finger actually produces in the page (touch vs pointer vs mouse),
    with coordinates, so we can see whether left/right halves are distinguishable in page space;
  * the maximum simultaneous `touches.length` Blink sees, which is the multitouch answer;
  * whether Steam or our own TouchModeGuard fights us for the mode atom.

PREREQUISITE, and the probe refuses to run without it: the app must be running with
`disable_touch: false`. With it on, `no_pointer.js` installs a document-start capture listener that
calls `stopImmediatePropagation()`, so a listener this probe registers later on the same target and
phase **never runs** — the probe would see zero events and report a false negative. Turning it off
also stops `TouchModeGuard`, which would otherwise yank the mode back to hover every 750 ms.

This probe swallows every event it observes, so taps cannot navigate Leanback while it runs.

Exit codes (.internal/HARNESS.md §1):

    0  the probe ran and produced an answer -- including "passthrough does not reach Blink", which is
       a FINDING to register, not a regression
    2  ASSERT     an invariant of the probe itself broke (the mode would not hold at all)
    3  ENV        cannot observe: disable_touch is on, our window is not focused, or nobody tapped
    4  TRANSPORT  the Deck or its DevTools endpoint went away
    5  USAGE
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_REPO / "scripts"))
sys.path.insert(0, str(_REPO / "tests" / "deck"))

import cdp as cdplib  # noqa: E402
from lib import probes, ssh as sshlib  # noqa: E402

EX_OK, EX_FAIL, EX_ASSERT, EX_ENV, EX_TRANSPORT, EX_USAGE = 0, 1, 2, 3, 4, 5

MODE_ATOM = "STEAM_TOUCH_CLICK_MODE"

# gamescope src/main.cpp: "--default-touch-mode 0: hover, 1: left, 2: right, 3: middle,
# 4: passthrough". 5 (disabled) exists in the enum but is not offered on the command line.
MODE_NAMES = {
    0: "hover",
    1: "left-click",
    2: "right-click",
    3: "middle-click",
    4: "passthrough",
    5: "disabled",
}

TOUCH_TYPES = ("touchstart", "touchmove", "touchend", "touchcancel")
POINTER_TYPES = ("pointerdown", "pointerup", "pointermove", "pointercancel")
MOUSE_TYPES = ("mousedown", "mouseup", "mousemove", "click", "dblclick", "contextmenu")

CAPS_EXPR = """JSON.stringify({
  maxTouchPoints: navigator.maxTouchPoints,
  ontouchstart: ('ontouchstart' in window),
  hasTouchEvent: (typeof window.TouchEvent !== 'undefined'),
  coarse: !!(window.matchMedia && matchMedia('(pointer: coarse)').matches),
  anyCoarse: !!(window.matchMedia && matchMedia('(any-pointer: coarse)').matches),
  viewport: [window.innerWidth, window.innerHeight]
})"""

UNINSTALL_EXPR = """(function(){
  try { if (window.__dbTouchProbe) window.__dbTouchProbe.uninstall(); } catch (e) {}
  try { delete window.__dbTouchProbe; } catch (e) { window.__dbTouchProbe = null; }
  return true;
})()"""

# Self-validating check for the no_pointer.js blindness described in the module docstring. A DETACHED
# node's own listener still fires (so `detached == 1` proves event dispatch works at all); an IN-TREE
# dispatch that never reaches our window-capture listener means something registered earlier is
# calling stopImmediatePropagation(), and every measurement below would read zero for the wrong
# reason. Same shape as tests/deck/test_touch.py's probe, inverted: there a 0 is the pass, here it is
# the abort.
SWALLOW_SELFTEST_EXPR = """(function(){
  var detached = 0, seen = 0;
  var d = document.createElement('div');
  d.addEventListener('pointerdown', function(){ detached++; });
  d.dispatchEvent(new PointerEvent('pointerdown'));
  var probe = function(){ seen++; };
  window.addEventListener('pointerdown', probe, true);
  try {
    (document.body || document.documentElement).dispatchEvent(
      new PointerEvent('pointerdown', {bubbles: true, cancelable: true}));
  } catch (e) {}
  window.removeEventListener('pointerdown', probe, true);
  return JSON.stringify({detached: detached, seen: seen});
})()"""

RECORDER_EXPR = """(function(){
  var TYPES = ['touchstart','touchmove','touchend','touchcancel',
               'pointerdown','pointerup','pointermove','pointercancel',
               'mousedown','mouseup','mousemove','click','dblclick','contextmenu'];
  var MAX_DOWNS = 40;
  try { if (window.__dbTouchProbe) window.__dbTouchProbe.uninstall(); } catch (e) {}
  var st;
  var fresh = function(){
    return {counts: {}, maxTouches: 0, ids: {}, downs: [], firstMs: -1, lastMs: -1};
  };
  st = fresh();
  var coord = function(v){ return (typeof v === 'number') ? Math.round(v) : null; };
  var onEvt = function(e){
    st.counts[e.type] = (st.counts[e.type] || 0) + 1;
    var ms = Math.round(e.timeStamp || 0);
    if (st.firstMs < 0) st.firstMs = ms;
    st.lastMs = ms;
    if (e.touches && typeof e.touches.length === 'number' && e.touches.length > st.maxTouches)
      st.maxTouches = e.touches.length;
    var ct = e.changedTouches;
    if (ct) { for (var i = 0; i < ct.length; i++) st.ids['' + ct[i].identifier] = 1; }
    if (e.type === 'touchstart' || e.type === 'pointerdown' || e.type === 'mousedown') {
      var x = null, y = null;
      if (ct && ct.length) { x = ct[0].clientX; y = ct[0].clientY; }
      else { x = e.clientX; y = e.clientY; }
      if (st.downs.length < MAX_DOWNS)
        st.downs.push({t: e.type, x: coord(x), y: coord(y), ms: ms});
    }
    // Swallow, so a tap cannot navigate Leanback while we measure. This is also a live rehearsal of
    // the gesture router: capture phase, stopImmediatePropagation, page never sees it.
    try { e.stopImmediatePropagation(); } catch (_) {}
    try { if (e.cancelable) e.preventDefault(); } catch (_) {}
  };
  var targets = [window, document];
  var each = function(fn){
    for (var i = 0; i < targets.length; i++)
      for (var j = 0; j < TYPES.length; j++) { try { fn(targets[i], TYPES[j]); } catch (_) {} }
  };
  each(function(t, type){ t.addEventListener(type, onEvt, {capture: true, passive: false}); });
  window.__dbTouchProbe = {
    uninstall: function(){ each(function(t, type){ t.removeEventListener(type, onEvt, true); }); },
    reset: function(){ st = fresh(); },
    report: function(){
      var ids = 0, k;
      for (k in st.ids) ids++;
      return {counts: st.counts, maxTouches: st.maxTouches, touchIds: ids, downs: st.downs,
              spanMs: (st.firstMs < 0 ? 0 : st.lastMs - st.firstMs),
              viewport: [window.innerWidth, window.innerHeight]};
    }
  };
  return true;
})()"""

RESET_EXPR = "(function(){ window.__dbTouchProbe.reset(); return true; })()"
REPORT_EXPR = "JSON.stringify(window.__dbTouchProbe.report())"


def parse_xprop_cardinal(text):
    """The integer in `xprop -root NAME` output, or None when unset/absent.

    `xprop` answers either `STEAM_TOUCH_CLICK_MODE(CARDINAL) = 4` or
    `STEAM_TOUCH_CLICK_MODE:  not found.`, and an unreachable X server answers nothing at all. All
    three must be distinguishable: "not found" is a real state (nobody has set the mode), while a
    parse failure means the reading is worthless.
    """
    if not text:
        return None
    m = re.search(r"=\s*(-?\d+)", text)
    return int(m.group(1)) if m else None


def classify(counts):
    """Which event family a sample actually produced: 'touch', 'pointer', 'mouse', or 'none'.

    Ordered by information content, not by count. A single `touchstart` beats a thousand
    `mousemove`s, because only the former proves real `wl_touch` survived the trip to Blink.
    """
    if any(counts.get(t, 0) for t in TOUCH_TYPES):
        return "touch"
    if any(counts.get(t, 0) for t in POINTER_TYPES):
        return "pointer"
    if any(counts.get(t, 0) for t in MOUSE_TYPES):
        return "mouse"
    return "none"


def split_sides(downs, width):
    """(left, right, middle) counts of press coordinates against a viewport width.

    The point is not the tally but whether coordinates arrive in PAGE space at all. If gamescope's
    letterbox transform were ours to do, these would cluster wrongly or exceed the viewport.
    """
    left = right = middle = 0
    for d in downs:
        x = d.get("x")
        if x is None or not width:
            continue
        if x < width * 0.35:
            left += 1
        elif x > width * 0.65:
            right += 1
        else:
            middle += 1
    return left, right, middle


def verdict(samples, caps):
    """(exit_code, headline, lines) from the per-mode samples. Pure, so it is L0-testable.

    A negative answer is still a successful probe: "passthrough does not reach Blink" is a finding to
    register, not a product regression. Only an inability to OBSERVE is an error here.
    """
    lines = []
    seen_anything = any(classify(s.get("counts", {})) != "none" for s in samples.values())
    if not seen_anything:
        return (
            EX_ENV,
            "INCONCLUSIVE — not one event reached the page in any mode.",
            [
                "Nothing was observed, so this run says nothing about passthrough.",
                "Likely causes, in order: nobody tapped; our window was not the focused one on the",
                "display we wrote the atom to; the app is running with disable_touch on after all.",
            ],
        )

    pt = samples.get(4, {})
    pt_counts = pt.get("counts", {})
    if classify(pt_counts) == "touch":
        max_touches = pt.get("maxTouches", 0)
        head = f"PASSTHROUGH WORKS — real touch events reach Blink (max {max_touches} simultaneous)."
        lines.append("Mode 4 delivers wl_touch through Xwayland/XI2 into Blink. Build the gesture")
        lines.append("layer on real touchstart/touchmove/touchend with per-finger identifiers.")
        if max_touches >= 2:
            lines.append(f"Multitouch confirmed at {max_touches} points: swipes and pinch are on the table.")
        else:
            lines.append("Only ONE simultaneous point was seen — re-run the multi-finger stage before")
            lines.append("designing anything that needs two fingers.")
        lines.append("Mode 4 is also the safer disable_touch default: no pointer emulation at all,")
        lines.append("so with the router uninstalled a stray finger does nothing and moves no cursor.")
        return (EX_OK, head, lines)

    ptr = samples.get(1, {})
    if classify(ptr.get("counts", {})) in ("pointer", "mouse"):
        head = "PASSTHROUGH DOES NOT REACH BLINK — fall back to the pointer router (mode 1)."
        lines.append("Mode 4 produced no touch events, so Xwayland/XI2 is not carrying wl_touch into")
        lines.append("this engine. Mode 1 does produce presses, so the gesture layer is still")
        lines.append("buildable — single-touch only, on pointerdown/pointerup, never click.")
        if not caps.get("maxTouchPoints"):
            lines.append("")
            lines.append("navigator.maxTouchPoints == 0: Chromium detected no touchscreen. Before")
            lines.append("accepting this verdict, retry with these in cobalt_flags and re-run:")
            lines.append("    --touch-events=enabled")
            lines.append("    --touch-devices=<XI id from `DISPLAY=<disp> xinput list`>")
            lines.append("(ui/events/event_switches.cc:28, applied at")
            lines.append(" ui/ozone/platform/x11/ozone_platform_x11.cc:266 in the pinned tree.)")
        return (EX_OK, head, lines)

    return (
        EX_OK,
        "PARTIAL — events were seen, but neither mode 4 nor mode 1 gave a usable press stream.",
        ["Read the per-mode table above and register what was actually observed."],
    )


def _say(msg=""):
    print(msg, file=sys.stderr, flush=True)


def _ask(prompt):
    _say("")
    _say(f">>> {prompt}")
    _say(">>> press Enter here when done (Ctrl-C aborts)")
    try:
        sys.stdin.readline()
    except KeyboardInterrupt:
        raise SystemExit(EX_OK)


def find_display(sh):
    """The Xwayland display where OUR content_shell is focused.

    SteamOS runs `--xwayland-count 2`: Steam on :0, a Steam-launched game on :1. The mode atom must
    be written on the display our window lives on; a naive :0 check always finds Steam and would
    silently probe the wrong compositor state (durable/touch-lock.md).
    """
    for disp in (":0", ":1", ":2"):
        _, cls, _ = sh.run(
            f"DISPLAY={disp} xdotool getwindowfocus getwindowclassname 2>/dev/null", check=False
        )
        if "content_shell" in (cls or "").lower():
            return disp
    return None


def read_mode(sh, disp):
    _, out, _ = sh.run(f"DISPLAY={disp} xprop -root {MODE_ATOM}", check=False)
    return parse_xprop_cardinal(out)


def set_mode(sh, disp, mode):
    sh.run(
        f"DISPLAY={disp} xprop -root -f {MODE_ATOM} 32c -set {MODE_ATOM} {mode}",
        check=False,
    )
    return read_mode(sh, disp)


def mode_held(sh, disp, mode, seconds=2.0):
    """Did the mode we set survive? Something else may own this atom.

    Steam manages it, and our own TouchModeGuard re-asserts hover every 750 ms when disable_touch is
    on. Either would silently invalidate a whole sample, so we watch rather than assume.
    """
    deadline = time.time() + seconds
    while time.time() < deadline:
        time.sleep(0.5)
        if read_mode(sh, disp) != mode:
            return False
    return True


def describe_environment(sh, disp):
    _say(f"display        {disp}")
    _, ver, _ = sh.run("gamescope --version 2>&1 | head -1", check=False)
    _say(f"gamescope      {(ver or '').strip() or 'unknown'}")
    _, modes, _ = sh.run("gamescope --help 2>&1 | grep -i 'touch-mode' || true", check=False)
    if (modes or "").strip():
        _say(f"modes          {modes.strip()}")
    rc, xin, _ = sh.run(f"DISPLAY={disp} xinput list 2>/dev/null || true", check=False)
    if (xin or "").strip():
        hits = [ln.strip() for ln in xin.splitlines() if re.search(r"touch|FTS", ln, re.I)]
        _say(f"xinput touch   {hits or 'no touch-looking device in `xinput list`'}")
    else:
        _say("xinput touch   unknown (xinput not installed on the Deck)")


def wait_for_taps(instruction, dwell):
    """Give the human a window to tap in: an Enter key if there is a terminal, else a timed one.

    `--dwell` exists because the operator and the terminal are not always the same party. Driven
    from an agent or a CI shell there is no stdin to press Enter on, and `_ask` would read EOF
    instantly and sample an empty window -- which `verdict()` correctly reports as ENV, but only
    after wasting the human's taps. A fixed window is honest about what it is: sample for N seconds,
    then report whatever arrived.
    """
    if not dwell:
        _ask(instruction)
        return
    _say("")
    _say(f">>> {instruction}")
    _say(f">>> sampling for {dwell:g}s starting NOW")
    time.sleep(dwell)
    _say(">>> window closed")


def sample_mode(cdp, sh, disp, mode, instruction, settle, dwell=0):
    _say("")
    _say(f"--- mode {mode} ({MODE_NAMES.get(mode, '?')}) " + "-" * 40)
    got = set_mode(sh, disp, mode)
    if got != mode:
        _say(f"    could not set the mode (atom reads {got!r}) — skipping this sample")
        return None
    if not mode_held(sh, disp, mode, settle):
        _say(f"    mode did NOT hold at {mode} — something is fighting us for the atom.")
        _say("    Is the app running with disable_touch on? TouchModeGuard re-asserts hover.")
        return None
    cdp.evaluate(RESET_EXPR)
    wait_for_taps(instruction, dwell)
    raw = cdp.evaluate(REPORT_EXPR)
    if not raw:
        raise ConnectionError("the page stopped answering (engine restarted?)")
    rep = json.loads(raw)
    counts = rep.get("counts", {})
    kind = classify(counts)
    width = (rep.get("viewport") or [0, 0])[0]
    left, right, middle = split_sides(rep.get("downs", []), width)
    _say(f"    family     {kind}")
    _say(f"    counts     {counts or '{}'}")
    _say(f"    presses    left={left} middle={middle} right={right} (viewport width {width})")
    if rep.get("maxTouches"):
        _say(f"    touches    max simultaneous {rep['maxTouches']}, distinct ids {rep.get('touchIds', 0)}")
    for d in rep.get("downs", [])[:8]:
        _say(f"      {d['t']:<12} x={d['x']} y={d['y']} t={d['ms']}ms")
    return rep


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--deck-host", default=None, help="override DECK_HOST")
    ap.add_argument(
        "--modes",
        default="4,1,0",
        help="touch modes to sample, in order (default 4,1,0 — passthrough first)",
    )
    ap.add_argument(
        "--settle",
        type=float,
        default=2.0,
        help="seconds to watch that a mode held before sampling (default 2)",
    )
    ap.add_argument(
        "--skip-multitouch", action="store_true", help="skip the two/three-finger stage"
    )
    ap.add_argument(
        "--dwell",
        type=float,
        default=0.0,
        help="sample each mode for N seconds instead of waiting on Enter. For when whoever runs "
             "this and whoever taps the panel are not the same party (an agent, a CI shell): with "
             "no stdin, the Enter prompt reads EOF instantly and samples an empty window.",
    )
    args = ap.parse_args(argv)
    if args.dwell < 0:
        _say("error: --dwell must not be negative")
        return EX_USAGE

    try:
        modes = [int(m) for m in args.modes.split(",") if m.strip() != ""]
    except ValueError:
        _say(f"error: --modes must be integers, got {args.modes!r}")
        return EX_USAGE
    if not modes:
        _say("error: --modes is empty")
        return EX_USAGE

    host = args.deck_host or sshlib.deck_host()
    port = sshlib.deck_port()
    if not host or not sshlib.reachable(host, port):
        _say(f"error: no Deck reachable at {host or '<unset DECK_HOST>'}:{port}")
        return EX_TRANSPORT
    sh = sshlib.Ssh(host, port)

    _say("=" * 72)
    _say("touch passthrough probe — needs a human with a finger on the Deck's panel")
    _say("=" * 72)

    disp = find_display(sh)
    if not disp:
        _say("error: our content_shell is not the focused window on :0, :1 or :2.")
        _say("       Foreground the app in Game Mode and re-run. (An SSH launch that forces")
        _say("       DISPLAY=:0 puts it on the wrong display — durable/touch-lock.md.)")
        return EX_ENV
    describe_environment(sh, disp)

    original = read_mode(sh, disp)
    _say(f"mode at start  {original!r} ({MODE_NAMES.get(original, 'unset')})")

    try:
        with sshlib.Tunnel(host, port, 9222) as tun:
            with cdplib.CDP(tun.cdp_port) as cdp:
                if not cdp.wait_for(probes.ON_LEANBACK_EXPR, timeout=45):
                    _say("error: the engine is not on youtube.com/tv (is the app running?)")
                    return EX_ENV

                caps = json.loads(cdp.evaluate(CAPS_EXPR) or "{}")
                _say("")
                _say("--- what Chromium thinks of the panel " + "-" * 34)
                _say(f"    navigator.maxTouchPoints   {caps.get('maxTouchPoints')}")
                _say(f"    'ontouchstart' in window   {caps.get('ontouchstart')}")
                _say(f"    window.TouchEvent          {caps.get('hasTouchEvent')}")
                _say(f"    (pointer: coarse)          {caps.get('coarse')}")
                _say(f"    (any-pointer: coarse)      {caps.get('anyCoarse')}")
                _say(f"    viewport                   {caps.get('viewport')}")
                if not caps.get("maxTouchPoints"):
                    _say("    ^ zero means Chromium found no Direct-mode XI2 touch device. Touch")
                    _say("      events may still not be dispatched even if mode 4 sends them.")

                cdp.evaluate(UNINSTALL_EXPR)
                st = json.loads(cdp.evaluate(SWALLOW_SELFTEST_EXPR) or "{}")
                if st.get("detached") != 1:
                    _say("")
                    _say("error: event dispatch is broken in the page — every reading below would be")
                    _say(f"       meaningless (detached control returned {st.get('detached')!r}).")
                    return EX_ENV
                if st.get("seen", 0) == 0:
                    _say("")
                    _say("error: something upstream is swallowing pointer events before this probe")
                    _say("       can see them — almost certainly no_pointer.js, i.e. the app is")
                    _say("       running with disable_touch ON. This probe would read zero for the")
                    _say("       wrong reason, so it refuses to guess.")
                    _say("")
                    _say("       Set \"disable_touch\": false in the app's app.json, restart the app,")
                    _say("       and re-run. That also stops TouchModeGuard fighting us for the mode.")
                    return EX_ENV

                if not cdp.evaluate(RECORDER_EXPR):
                    _say("error: could not install the recorder in the page")
                    return EX_ENV
                _say("")
                _say("recorder installed — it swallows what it records, so taps cannot navigate.")

                samples = {}
                for mode in modes:
                    where = "LEFT third, then 3 taps on the RIGHT third"
                    rep = sample_mode(
                        cdp,
                        sh,
                        disp,
                        mode,
                        f"tap the screen 3 times on the {where} (6 taps total)",
                        args.settle,
                        args.dwell,
                    )
                    if rep is not None:
                        samples[mode] = rep

                if not args.skip_multitouch and classify(
                    samples.get(4, {}).get("counts", {})
                ) == "touch":
                    rep = sample_mode(
                        cdp,
                        sh,
                        disp,
                        4,
                        "now press with TWO fingers at once, lift, then THREE fingers at once",
                        args.settle,
                        args.dwell,
                    )
                    if rep is not None:
                        samples[4]["maxTouches"] = max(
                            samples[4].get("maxTouches", 0), rep.get("maxTouches", 0)
                        )
                        samples[4]["touchIds"] = max(
                            samples[4].get("touchIds", 0), rep.get("touchIds", 0)
                        )

                cdp.evaluate(UNINSTALL_EXPR)

                code, head, lines = verdict(samples, caps)
                _say("")
                _say("=" * 72)
                _say(head)
                _say("=" * 72)
                for ln in lines:
                    _say(ln)
                return code
    except (sshlib.NoDevTools, sshlib.DeckUnreachable, ConnectionError, TimeoutError) as e:
        _say(f"error: transport: {e}")
        return EX_TRANSPORT
    except KeyboardInterrupt:
        _say("")
        _say("aborted")
        return EX_OK
    finally:
        if original is not None:
            set_mode(sh, disp, original)
            _say(f"restored {MODE_ATOM} to {original}")
        else:
            _say(f"note: {MODE_ATOM} was unset at start; left at its last probed value")


if __name__ == "__main__":
    sys.exit(main())
