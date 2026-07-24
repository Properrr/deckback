"""`disable_touch`: is the touchscreen actually inert on the shipped app?

On the Deck under gamescope the panel arrives as synthetic MOUSE events — a finger moves the cursor
and a tap clicks, navigating YouTube by accident. The app cannot take the panel away from the
compositor: an `EVIOCGRAB` lock was tried, proven non-functional on SteamOS (the launcher cannot
even open the panel node), and has since been deleted rather than shipped disabled — see
`.internal/findings/durable/touch-lock.md`. The tests that re-proved that negative on every run are
gone with it.

What ships instead makes touch inert two independent ways, and this file asserts BOTH actually
engage on the deployed app:

    Option A  the launcher injects config/no_pointer.js at document start, swallowing every
              pointer/mouse/touch event in the page before a Leanback listener sees it.
    Option B  TouchModeGuard holds gamescope's global STEAM_TOUCH_CLICK_MODE at hover (0) while our
              window is focused, so a stray finger cannot even generate a click at the compositor.

The observable is Leanback's own state and gamescope's own property, never our bookkeeping
(TEST-PLAN §0).
"""

from __future__ import annotations

import pytest

# Self-validating probe for Option A. A DETACHED node's own listener still fires (proving the event
# machinery works, so a 0 below means 'swallowed', not 'dispatch is broken'); an IN-TREE dispatch must
# be eaten by the launcher's window-capture swallow (no_pointer.js) before any page listener sees it.
_DISABLE_TOUCH_PROBE = r"""(function(){
  var d = document.createElement('div'); var control = 0;
  d.addEventListener('click', function(){ control++; });
  d.dispatchEvent(new MouseEvent('click'));
  var seen = 0; var probe = function(){ seen++; };
  window.addEventListener('click', probe, true);
  document.addEventListener('pointerdown', probe, true);
  document.body.dispatchEvent(new MouseEvent('click', {bubbles:true, cancelable:true}));
  document.body.dispatchEvent(new PointerEvent('pointerdown', {bubbles:true, cancelable:true}));
  window.removeEventListener('click', probe, true);
  document.removeEventListener('pointerdown', probe, true);
  return JSON.stringify({control: control, seen: seen});
})()"""


@pytest.mark.gate
def test_disable_touch_swallows_pointer_events(leanback):
    """Option A: no_pointer.js swallows pointer/mouse/touch in the page, so a finger — delivered as
    synthetic MOUSE events under gamescope — cannot navigate. Self-validating via the detached-node
    control, so `seen == 0` means swallowed, not that dispatch is broken."""
    import json

    raw = leanback.evaluate(_DISABLE_TOUCH_PROBE)
    assert raw, "probe returned nothing (no <body>? engine unreachable?)"
    r = json.loads(raw)
    assert r["control"] == 1, (
        f"event machinery is broken (control={r['control']}); the swallow result below would be "
        "meaningless, so this run is inconclusive, not a pass"
    )
    assert r["seen"] == 0, (
        f"{r['seen']} in-tree pointer event(s) reached the page — no_pointer.js is NOT swallowing "
        "them. Is disable_touch off, or did the document-start injection fail?"
    )


@pytest.mark.probe
def test_gamescope_touch_mode_is_hover_while_focused(deck_host):
    """Option B: TouchModeGuard holds gamescope's STEAM_TOUCH_CLICK_MODE at 0 (hover) while OUR window
    is focused. Skips — never fails — when our app is not the focused window, because the guard
    deliberately leaves the mode alone then.

    gamescope runs several Xwayland servers (--xwayland-count 2): Steam on :0, a Steam-launched game
    on :1. Our app — and the touch mode the guard manages — live on whichever display our
    content_shell is FOCUSED on, which is why a naive :0 check always sees 'steam' and skips."""
    from lib import ssh as sshlib

    sh = sshlib.Ssh(deck_host, sshlib.deck_port())
    target = None
    for disp in (":0", ":1", ":2"):
        _, cls, _ = sh.run(
            f"DISPLAY={disp} xdotool getwindowfocus getwindowclassname 2>/dev/null", check=False
        )
        if "content_shell" in (cls or "").lower():
            target = disp
            break
    if not target:
        pytest.skip(
            "our content_shell is not the focused window on any Xwayland display (app backgrounded or "
            "launched over SSH onto the wrong display); the guard only asserts hover while focused"
        )
    _, mode, _ = sh.run(f"DISPLAY={target} xprop -root STEAM_TOUCH_CLICK_MODE", check=False)
    assert "= 0" in (mode or ""), (
        f"our window is focused on {target} but gamescope touch mode is not hover: {mode.strip()!r}. "
        "TouchModeGuard should be holding STEAM_TOUCH_CLICK_MODE at 0."
    )
