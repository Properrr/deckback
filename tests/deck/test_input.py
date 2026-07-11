"""The launcher's evdev -> CDP path, and Leanback's response to it.

Two channels, and the difference is the whole design of this file:

  CDP key injection bypasses evdev, Steam Input and gamescope. A pass tells you about **Leanback**.
  uinput injection goes through all of them. A pass tells you about **our launcher**.

So a CDP test is the positive control for a uinput test: if `ArrowDown` over CDP does not move focus,
then a uinput D-pad press failing to move focus indicts nothing — the assertion method itself is
broken. Order matters, and pytest runs top to bottom.

Never assert that focus moved *by one*. Steam Input may re-wrap a newly created uinput pad and
deliver every press twice (TEST-PLAN §3, open question). Assert that focus **changed**.
"""

from __future__ import annotations

import time

import pytest

from lib import probes
from lib import uinput as u

SETTLE_S = 0.6


def _focus(cdp):
    return cdp.evaluate(probes.ACTIVE_ELEMENT_EXPR)


@pytest.mark.gate
def test_cdp_arrow_moves_focus(leanback):
    """The positive control for everything below. Proves the *assertion method* works.

    Also the R1 canary: if Leanback stops moving focus on ArrowDown, our whole input model is void
    regardless of how the key got there.
    """
    before = _focus(leanback)
    leanback.dispatch_key("ArrowDown")
    time.sleep(SETTLE_S)
    after = _focus(leanback)
    assert after != before, (
        f"CDP ArrowDown did not move focus (still {after!r}). Either nothing is focusable on this "
        "surface, or Leanback no longer navigates on arrows — check before blaming the launcher."
    )


@pytest.mark.gate
def test_cdp_keys_are_trusted(leanback):
    """`isTrusted` is what makes Leanback act on the event at all (S0.6)."""
    leanback.evaluate(
        "(function(){window.__dbTrusted=null;"
        "document.addEventListener('keydown',function(e){window.__dbTrusted=e.isTrusted;},"
        "{once:true,capture:true});return true;})()"
    )
    leanback.dispatch_key("ArrowUp")
    time.sleep(SETTLE_S)
    assert leanback.evaluate("window.__dbTrusted") is True, (
        "the injected keydown was not trusted; Leanback ignores untrusted keys"
    )


@pytest.mark.gate
@pytest.mark.uinput
def test_uinput_dpad_moves_focus(deck, leanback):
    """The launcher read a real evdev event and dispatched a real key. Nothing else can do this.

    A pass proves the entire Phase 3 chain: /dev/input -> GamepadInput -> CDP -> Leanback.
    """
    before = _focus(leanback)

    pad = u.gamepad_spec()
    # settle=2.5s: the launcher rescans for new pads on a 2 s hotplug tick (input.cpp
    # kHotplugScanMs), so a device created and destroyed faster than that is never seen.
    rc, _, err = deck.python(u.remote_program(pad, u.dpad(0, 1), settle=2.5), check=False)
    if rc == 3:
        pytest.skip(f"/dev/uinput unusable (environment): {err.strip()}")
    assert rc == 0, f"synthetic pad failed (rc={rc}): {err.strip()}"

    time.sleep(SETTLE_S)
    after = _focus(leanback)
    assert after != before, (
        f"a synthetic D-pad Down did not move focus (still {after!r}). CDP arrows do work "
        "(see test_cdp_arrow_moves_focus), so the break is in the launcher's evdev path: it either "
        "never opened the new pad (hotplug scan), or never dispatched."
    )


@pytest.mark.gate
@pytest.mark.uinput
def test_uinput_button_a_activates(deck, leanback):
    """A -> Enter -> Leanback activates the focused tile. Asserts the route changed, not that we sent."""
    before = leanback.evaluate("location.href")
    pad = u.gamepad_spec()
    rc, _, err = deck.python(u.remote_program(pad, u.press(u.BTN_SOUTH), settle=2.5), check=False)
    if rc == 3:
        pytest.skip(f"/dev/uinput unusable: {err.strip()}")
    assert rc == 0, err

    # Activation navigates (to a watch route, a shelf, a menu). Poll rather than sleep a guess.
    changed = leanback.wait_for(f"location.href !== {before!r}", timeout=10)
    assert changed, (
        f"A (BTN_SOUTH) did not change the route from {before!r}. Either the launcher did not "
        "dispatch Enter, or nothing was focused to activate."
    )


@pytest.mark.probe
@pytest.mark.uinput
def test_right_stick_scrolls_focus(deck, leanback):
    """input-ux §15. Requires ABS_RX/ABS_RY to exist at all — see test_capabilities.py.

    Full deflection held briefly should produce several arrow bursts, so focus moves. We assert it
    changed, never how far: the repeat rate is time-based and the pad's own report cadence is not
    ours to predict.
    """
    before = _focus(leanback)
    pad = u.gamepad_spec()
    events = u.stick(u.ABS_RY, 32767)
    # Hold: the launcher repeats on its own timer while the axis stays deflected, so simply not
    # re-centering for a moment is the whole "hold".
    events += [(u.EV_ABS, u.ABS_RY, 32767)] + u.syn()
    events += u.stick(u.ABS_RY, 0)
    rc, _, err = deck.python(u.remote_program(pad, events, settle=2.5), check=False)
    if rc == 3:
        pytest.skip(f"/dev/uinput unusable: {err.strip()}")
    assert rc == 0, err

    time.sleep(SETTLE_S)
    assert _focus(leanback) != before, (
        "right stick full-down did not move focus. Check test_right_stick_axes_exist first: if the "
        "virtual pad has no ABS_RY, this feature cannot work and the finding is there, not here."
    )
