"""What the Deck's input devices can do: right stick, analog triggers, rumble, uinput access.

`probe` failures are findings to register, not regressions; never mark them xfail.
"""

from __future__ import annotations

import json

import pytest

from lib import uinput as u

# Steam Input's virtual pad in Game Mode. Matched by name, never by eventN.
VIRTUAL_PAD_NAME = "X-Box 360 pad"


@pytest.fixture(scope="module")
def devices(deck):
    rc, out, err = deck.python(u.capabilities_program(), check=False)
    if rc != 0:
        pytest.skip(f"capability probe failed on the Deck (rc={rc}): {err.strip()}")
    return json.loads(out)


def _pad(devices):
    pads = u.find_device(devices, name_contains=VIRTUAL_PAD_NAME)
    if not pads:
        # Fall back to any gamepad-shaped device so a failure reports what IS there.
        pads = u.find_device(devices, needs_abs=[u.ABS_X, u.ABS_Y], needs_ev=[u.EV_KEY])
    return pads


@pytest.mark.gate
def test_a_gamepad_exists(devices):
    pads = _pad(devices)
    assert pads, (
        "no gamepad-capable /dev/input node found. Every Phase 3 test below is meaningless "
        f"until this passes. Devices seen: {[d['name'] for d in devices]}"
    )


@pytest.mark.probe
def test_right_stick_axes_exist(devices):
    """The pad exposes ABS_RX/ABS_RY, which right-stick fast scroll reads."""
    pads = _pad(devices)
    assert pads, "no pad to probe"
    have = [d for d in pads if u.ABS_RX in d["abs"] and u.ABS_RY in d["abs"]]
    assert have, (
        "no pad exposes ABS_RX/ABS_RY — right-stick fast scroll cannot work. "
        f"Pads and their ABS axes: {[(d['name'], [hex(a) for a in d['abs']]) for d in pads]}"
    )


@pytest.mark.probe
def test_trigger_axes_are_analog(devices):
    """The pad exposes analog ABS_Z/ABS_RZ triggers, which `input.cpp` reads."""
    pads = _pad(devices)
    assert pads, "no pad to probe"
    have = [d for d in pads if u.ABS_Z in d["abs"] and u.ABS_RZ in d["abs"]]
    assert have, (
        "no pad exposes analog ABS_Z/ABS_RZ triggers. `trigger_pressed()`'s hysteresis path never "
        "runs, and L2/R2 are dead independently of their (also missing) key bindings. "
        f"Pads: {[(d['name'], [hex(a) for a in d['abs']]) for d in pads]}"
    )


@pytest.mark.probe
def test_pad_supports_force_feedback(devices):
    """The pad advertises EV_FF/FF_RUMBLE for the Exit-hold haptic."""
    pads = _pad(devices)
    assert pads, "no pad to probe"
    have = [d for d in pads if u.EV_FF in d["ev"] and u.FF_RUMBLE in d["ff"]]
    assert have, (
        "no pad advertises EV_FF/FF_RUMBLE — the Exit-hold rumble is a silent no-op and the "
        "on-screen fill is the only feedback. This is a finding, not necessarily a bug. "
        f"Pads: {[(d['name'], d['ev'], d['ff']) for d in pads]}"
    )


@pytest.mark.probe
def test_uinput_is_writable(deck):
    """/dev/uinput is writable (udev rule `KERNEL=="uinput", MODE="0660", GROUP="input"`)."""
    rc, _, err = deck.python(
        u.remote_program(u.gamepad_spec("deckback-probe-pad"), events=(), settle=0.1), check=False
    )
    if rc == 3:
        pytest.skip(f"/dev/uinput not usable on the Deck (environment, not a defect): {err.strip()}")
    assert rc == 0, f"uinput device creation failed unexpectedly (rc={rc}): {err.strip()}"
