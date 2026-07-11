#!/usr/bin/env python3
# Host-side helper: keep the Deck awake while Deckback plays video by nudging a synthetic pointer
# every INTERVAL seconds, so gamescope/Steam's input-idle timer never fires (no screen-off, no
# auto-suspend). SteamOS's shortest sleep timer is 1 minute, so a sub-minute nudge holds it off.
#
# Why a host helper (not in the Flatpak): /dev/uinput is not in the sandbox, AND gamescope IGNORES
# sandbox-emulated input (libei/EIS) for idle purposes — only a real evdev device resets the timer
# (both verified on-Deck 2026-07-11: a uinput jiggle kept the screen on; a libei nudge did not).
#
# Gated on the launcher's own "playback active" logind inhibitor, so it nudges ONLY during playback
# (the Deck still sleeps normally when you're idle in a menu). The nudge is a pointer motion, which
# Deckback's no_pointer.js swallows in the page — so it never brings up YouTube's player controls.
#
# Self-cleaning: flatpak cannot run host commands on uninstall, so the helper watches for Deckback
# being removed and then uninstalls ITSELF — no service is left running on the host after Deckback is
# gone. (Even before that fires it is inert without Deckback, since the playback inhibitor is absent.)
import glob
import os
import subprocess
import sys
import time

from evdev import UInput, ecodes as e

APP = "io.github.properrr.deckback"
INTERVAL = int(sys.argv[1]) if len(sys.argv) > 1 else 25  # < SteamOS's 1-minute minimum sleep timer
HOME = os.path.expanduser("~")


def deckback_installed():
    """True if the Deckback Flatpak is present (user or system install)."""
    return bool(
        glob.glob(f"{HOME}/.local/share/flatpak/app/{APP}")
        or glob.glob(f"/var/lib/flatpak/app/{APP}")
    )


def deckback_playing():
    """True while Deckback holds its playback inhibitor (launcher/src/platform.cpp)."""
    try:
        out = subprocess.run(
            ["systemd-inhibit", "--list", "--no-pager"],
            capture_output=True, text=True, timeout=5,
        ).stdout
        return "playback active" in out
    except Exception:
        return False


def self_uninstall():
    """Deckback is gone — take the helper with it. Detached so it survives our own 'stop'."""
    subprocess.Popen(
        ["sh", "-c",
         "systemctl --user disable --now deckback-idle-nudge.service; "
         f"rm -f {HOME}/.local/bin/deckback-idle-nudge "
         f"{HOME}/.config/systemd/user/deckback-idle-nudge.service; "
         "systemctl --user daemon-reload"],
        start_new_session=True,
    )
    sys.exit(0)


def main():
    # REL_X/REL_Y + a button so libinput classifies it as a pointer (a rel-only device is ignored).
    cap = {e.EV_REL: [e.REL_X, e.REL_Y], e.EV_KEY: [e.BTN_LEFT]}
    ui = UInput(cap, name="deckback-idle-nudge")
    missing = 0
    try:
        while True:
            if deckback_installed():
                missing = 0
            else:
                # Debounced: a Flatpak UPDATE briefly churns the app dir; only self-remove once it has
                # been gone for several checks (a real uninstall), never mid-update.
                missing += 1
                if missing >= 3:
                    self_uninstall()  # exits

            if deckback_playing():
                ui.write(e.EV_REL, e.REL_X, 1)
                ui.syn()
                time.sleep(0.05)
                ui.write(e.EV_REL, e.REL_X, -1)  # net-zero displacement
                ui.syn()
            time.sleep(INTERVAL)
    finally:
        ui.close()


if __name__ == "__main__":
    main()
