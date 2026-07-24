"""What the Deck's input devices can actually do.

This one file settles three separate "unverified" entries in TEST-PLAN §2, all of which have been
blocking decisions for weeks and all of which are one evdev read away:

  * `ABS_RX`/`ABS_RY` — does Steam Input's virtual pad expose the right stick at all? The whole
    fast-scroll feature (input-ux §15) assumes it does.
  * `ABS_Z`/`ABS_RZ` — analog triggers, or did `config/steam_input.vdf`'s `"click" -> xinput_button
    TRIGGER_*` binding turn them into digital buttons? `input.cpp` reads the axes. If they are
    buttons, L2/R2 are dead for a second, independent reason (input-ux §12).
  * `EV_FF`/`FF_RUMBLE` — can we rumble the pad for the Exit-hold haptic (input-ux §14)?

These are `probe` tests: a failure is a **finding to register**, not a regression. But it is still a
failure, reported and visible. Marking them `xfail` would hide the answer, which is the only thing
they exist to produce.
"""

from __future__ import annotations

import json

import pytest

from lib import uinput as u

# Steam Input re-emits the physical controller as this virtual pad in Game Mode (input-ux §1,
# confirmed on-device 2026-07-08). Matched by name, never by eventN — node numbers move across boots.
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
        # Fall back to any gamepad-shaped device, so the test reports what IS there rather than
        # skipping. "No virtual pad" is itself a result worth seeing.
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
    """input-ux §15: fast scroll reads ABS_RX/ABS_RY. If they are absent, the feature is a no-op."""
    pads = _pad(devices)
    assert pads, "no pad to probe"
    have = [d for d in pads if u.ABS_RX in d["abs"] and u.ABS_RY in d["abs"]]
    assert have, (
        "no pad exposes ABS_RX/ABS_RY — right-stick fast scroll cannot work. "
        f"Pads and their ABS axes: {[(d['name'], [hex(a) for a in d['abs']]) for d in pads]}"
    )


@pytest.mark.probe
def test_trigger_axes_are_analog(devices):
    """input-ux §12 hazard: `input.cpp` reads ABS_Z/ABS_RZ, but the vdf binds a digital click."""
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
    """input-ux §14: the Exit-hold haptic uploads an FF_RUMBLE effect via EVIOCSFF."""
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
    """The udev rule the whole uinput half of this harness depends on.

    `KERNEL=="uinput", MODE="0660", GROUP="input"`. Without it every uinput test below fails with
    EACCES, and the failure looks like a product bug rather than a missing rule on the test host.
    """
    rc, _, err = deck.python(
        u.remote_program(u.gamepad_spec("deckback-probe-pad"), events=(), settle=0.1), check=False
    )
    if rc == 3:
        pytest.skip(f"/dev/uinput not usable on the Deck (environment, not a defect): {err.strip()}")
    assert rc == 0, f"uinput device creation failed unexpectedly (rc={rc}): {err.strip()}"
