"""Touch policy on the deployed app: page events swallowed, gamescope touch mode held.

Option A: no_pointer.js (or touch_gestures.js) swallows in-page pointer events. Option B:
TouchModeGuard holds STEAM_TOUCH_CLICK_MODE while our window is focused.
"""

from __future__ import annotations

import pytest

# Self-validating probe for Option A -> {control, seen}. control: a DETACHED node's listener fires
# (1 = dispatch works). seen: IN-TREE events that reached page capture listeners (0 = swallowed).
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


# The running touch policy: 'gestures' if the router's global exists, else 'lock'.
_TOUCH_POLICY_PROBE = """(function(){
  return window.__deckbackGestures ? 'gestures' : 'lock';
})()"""


def touch_policy(cdp_session):
    return cdp_session.evaluate(_TOUCH_POLICY_PROBE) or "lock"


@pytest.mark.gate
def test_touch_cannot_navigate_leanback_under_either_policy(leanback):
    """In-tree pointer events never reach the page under either touch policy."""
    import json

    policy = touch_policy(leanback)
    raw = leanback.evaluate(_DISABLE_TOUCH_PROBE)
    assert raw, "probe returned nothing (no <body>? engine unreachable?)"
    r = json.loads(raw)
    assert r["control"] == 1, (
        f"event machinery is broken (control={r['control']}); the swallow result below would be "
        "meaningless, so this run is inconclusive, not a pass"
    )
    culprit = (
        "no_pointer.js is NOT swallowing them — is disable_touch off, or did the document-start "
        "injection fail?"
        if policy == "lock"
        else "touch_gestures.js is NOT swallowing them — the router is installed but not consuming "
        "events, so a tap would both gesture AND navigate."
    )
    assert r["seen"] == 0, (
        f"{r['seen']} in-tree pointer event(s) reached the page under the '{policy}' policy. "
        f"{culprit}"
    )


@pytest.mark.probe
def test_gamescope_touch_mode_matches_the_configured_policy(deck_host, leanback):
    """STEAM_TOUCH_CLICK_MODE is 0 (hover) under the lock or 4 (passthrough) under gestures while
    our window is focused; skips when it is not focused on any Xwayland display (:0-:2).
    """
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
    policy = touch_policy(leanback)
    want = 4 if policy == "gestures" else 0
    name = "passthrough" if want == 4 else "hover"
    _, mode, _ = sh.run(f"DISPLAY={target} xprop -root STEAM_TOUCH_CLICK_MODE", check=False)
    assert f"= {want}" in (mode or ""), (
        f"our window is focused on {target} and the app is running the '{policy}' touch policy, "
        f"which needs STEAM_TOUCH_CLICK_MODE = {want} ({name}) — but it reads {mode.strip()!r}. "
        "TouchModeGuard should be holding it there while we are focused."
    )
