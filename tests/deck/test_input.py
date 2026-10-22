"""The launcher's evdev -> CDP path, and Leanback's response to it.

CDP tests run first as the positive control for the uinput tests. Assert that focus changed, never
by how much (Steam Input may double-deliver presses).
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
    """CDP ArrowDown moves Leanback's focus (the positive control for the uinput tests)."""
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
    """CDP-injected keydowns are `isTrusted`, which Leanback requires."""
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
    """A uinput D-pad press moves focus: /dev/input -> GamepadInput -> CDP -> Leanback."""
    before = _focus(leanback)

    pad = u.gamepad_spec()
    # settle=2.5s: the launcher rescans for new pads on a 2 s hotplug tick (kHotplugScanMs).
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
    """A uinput A press activates the focused tile (the route changes)."""
    if leanback.evaluate(probes.ACCOUNT_PICKER_EXPR):
        pytest.skip("Leanback is on the account picker (environment): pick a profile on the Deck first")
    before = leanback.evaluate("location.href")
    pad = u.gamepad_spec()
    rc, _, err = deck.python(u.remote_program(pad, u.press(u.BTN_SOUTH), settle=2.5), check=False)
    if rc == 3:
        pytest.skip(f"/dev/uinput unusable: {err.strip()}")
    assert rc == 0, err

    changed = leanback.wait_for(f"location.href !== {before!r}", timeout=10)
    assert changed, (
        f"A (BTN_SOUTH) did not change the route from {before!r}. Either the launcher did not "
        "dispatch Enter, or nothing was focused to activate."
    )


@pytest.mark.probe
@pytest.mark.uinput
def test_right_stick_scrolls_focus(deck, leanback):
    """A held full right-stick deflection moves focus (needs ABS_RY; see test_capabilities.py)."""
    before = _focus(leanback)
    pad = u.gamepad_spec()
    events = u.stick(u.ABS_RY, 32767)
    # The launcher repeats on its own timer while the axis stays deflected.
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
