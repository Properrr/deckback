#!/usr/bin/env python3
# Host-side helper: keeps the Deck awake while Deckback plays video by nudging a synthetic pointer,
# so gamescope/Steam's input-idle timer never fires. Runs on the host because /dev/uinput is outside
# the Flatpak sandbox and gamescope ignores sandbox-emulated input for idle.
#
# Playback is detected from logind's inhibitor list over D-Bus, never by grepping
# `systemd-inhibit --list` (that table truncates the WHY column and silently loses the marker).
# Self-uninstalls once Deckback is gone. Rationale + on-Deck results:
# .internal/findings/durable/keep-awake.md
import argparse
import glob
import os
import shutil
import subprocess
import sys
import time

APP = "io.github.properrr.deckback"
# Fragment of the launcher's logind inhibitor "why" (launcher/src/platform.cpp).
INHIBIT_MARKER = "playback active"
DEFAULT_INTERVAL = 25   # seconds; must stay < SteamOS's 1-minute minimum sleep timer
MIN_INTERVAL = 5
MAX_INTERVAL = 55       # margin under the 60 s floor even if over-tuned
# Continuous absence before self-removal. Installing/updating Deckback is an uninstall immediately
# followed by an install, so a short grace period makes the helper delete itself mid-update — which
# is what disabled it on this Deck. It is inert without Deckback, so waiting costs nothing.
MISSING_GRACE_SECONDS = 900
HOME = os.path.expanduser("~")


def log(msg):
    """One line to stderr → the journal (`journalctl --user -u deckback-idle-nudge`)."""
    print(f"deckback-idle-nudge: {msg}", file=sys.stderr, flush=True)


def resolve_interval(cli_value=None):
    """CLI arg wins, then $DECKBACK_NUDGE_INTERVAL, then the default — clamped under the sleep floor."""
    for raw in (cli_value, os.environ.get("DECKBACK_NUDGE_INTERVAL")):
        if raw is None or raw == "":
            continue
        try:
            n = int(raw)
        except (TypeError, ValueError):
            continue
        return max(MIN_INTERVAL, min(n, MAX_INTERVAL))
    return DEFAULT_INTERVAL


def missing_limit(interval, grace=MISSING_GRACE_SECONDS):
    """Consecutive absent polls before self-removal, derived from a wall-clock grace period."""
    return max(3, grace // max(1, interval))


def deckback_installed(home=HOME):
    """True if the Deckback Flatpak is present (user or system install)."""
    return bool(
        glob.glob(f"{home}/.local/share/flatpak/app/{APP}")
        or glob.glob(f"/var/lib/flatpak/app/{APP}")
    )


def read_inhibitors():
    """Return (source, text) for logind's inhibitor list, preferring the untruncated D-Bus reply."""
    try:
        out = subprocess.run(
            ["busctl", "--json=short", "call",
             "org.freedesktop.login1", "/org/freedesktop/login1",
             "org.freedesktop.login1.Manager", "ListInhibitors"],
            capture_output=True, text=True, timeout=5,
        )
        if out.returncode == 0 and out.stdout.strip():
            return ("busctl", out.stdout)
    except Exception:
        pass
    try:
        env = dict(os.environ, COLUMNS="1000", SYSTEMD_COLORS="0")
        out = subprocess.run(
            ["systemd-inhibit", "--list", "--no-pager"],
            capture_output=True, text=True, timeout=5, env=env,
        )
        return ("systemd-inhibit", out.stdout)
    except Exception:
        return ("unavailable", "")


def inhibitor_active(text, marker=INHIBIT_MARKER):
    """True if the launcher's playback inhibitor appears in a logind inhibitor listing.

    Correct for both sources: each carries the WHY verbatim when untruncated, and the marker has no
    characters JSON would escape.
    """
    return marker in (text or "")


def deckback_playing():
    """True while Deckback holds its playback inhibitor (launcher/src/platform.cpp)."""
    _, text = read_inhibitors()
    return inhibitor_active(text)


def self_uninstall():
    """Deckback is gone — take the helper with it.

    The cleanup runs as a transient unit (its own cgroup) because it stops this very service, and
    KillMode=control-group would SIGTERM a plain detached child mid-cleanup.
    """
    cleanup = (
        "systemctl --user disable --now deckback-idle-nudge.service; "
        f"rm -f {HOME}/.local/bin/deckback-idle-nudge "
        f"{HOME}/.config/systemd/user/deckback-idle-nudge.service; "
        "systemctl --user daemon-reload"
    )
    if shutil.which("systemd-run"):
        subprocess.Popen(
            ["systemd-run", "--user", "--collect", "--quiet", "/bin/sh", "-c", cleanup],
            start_new_session=True,
        )
    else:
        subprocess.Popen(["sh", "-c", cleanup], start_new_session=True)
    sys.exit(0)


def _open_uinput():
    """Create the synthetic pointer. evdev is imported lazily so the detection logic stays importable
    where python-evdev is absent (CI)."""
    from evdev import UInput, ecodes as e
    # REL_X/REL_Y + a button so libinput classifies it as a pointer (a rel-only device is ignored).
    cap = {e.EV_REL: [e.REL_X, e.REL_Y], e.EV_KEY: [e.BTN_LEFT]}
    return UInput(cap, name="deckback-idle-nudge"), e


def _nudge(ui, e):
    """One net-zero jiggle, sent as two distinct SYN frames so the motion is not cancelled."""
    ui.write(e.EV_REL, e.REL_X, 1)
    ui.write(e.EV_REL, e.REL_Y, 1)
    ui.syn()
    time.sleep(0.05)
    ui.write(e.EV_REL, e.REL_X, -1)
    ui.write(e.EV_REL, e.REL_Y, -1)
    ui.syn()


def cmd_check():
    """Print whether playback is detected and from which source, then exit."""
    source, text = read_inhibitors()
    active = inhibitor_active(text)
    log(f"playback detected: {active} (source: {source}, marker: {INHIBIT_MARKER!r})")
    if not active:
        log("no Deckback playback inhibitor found — is a video actually playing, and is the launcher "
            "holding its 'playback active' inhibitor? (If a video IS playing and this still says "
            "False, the detection regressed — file it.)")
    print(text, end="" if text.endswith("\n") else "\n")
    return 0 if active else 1


def cmd_force(seconds, interval):
    """Nudge unconditionally for `seconds`, bypassing detection — separates a detection failure from
    gamescope no longer honoring the synthetic device."""
    ui, e = _open_uinput()
    log(f"forcing nudges every {interval}s for {seconds}s (playback detection bypassed)")
    deadline = seconds
    try:
        while deadline > 0:
            _nudge(ui, e)
            step = min(interval, deadline)
            time.sleep(step)
            deadline -= step
    finally:
        ui.close()
    log("force run complete")
    return 0


def run_daemon(interval):
    ui, e = _open_uinput()
    limit = missing_limit(interval)
    missing = 0
    was_playing = None
    log(f"started; interval={interval}s, marker={INHIBIT_MARKER!r}, "
        f"self-remove after {limit} absent polls (~{limit * interval}s)")
    try:
        while True:
            if deckback_installed():
                missing = 0
            else:
                missing += 1
                if missing == 1:
                    log("Deckback not found — starting the self-removal grace period")
                if missing >= limit:
                    log("Deckback absent for the full grace period; self-removing helper")
                    self_uninstall()  # exits

            source, text = read_inhibitors()
            playing = inhibitor_active(text)
            if playing != was_playing:
                log("playback active — nudging" if playing
                    else f"playback idle — nudges paused (source: {source})")
                was_playing = playing
            if playing:
                _nudge(ui, e)
            time.sleep(interval)
    finally:
        ui.close()


def main(argv=None):
    p = argparse.ArgumentParser(
        prog="deckback-idle-nudge",
        description="Keep the Steam Deck awake while Deckback plays a video.",
    )
    p.add_argument("interval", nargs="?", type=int, default=None,
                   help="seconds between nudges (default 25; also $DECKBACK_NUDGE_INTERVAL)")
    p.add_argument("--check", action="store_true",
                   help="print whether playback is detected + the raw inhibitor listing, then exit")
    p.add_argument("--force", nargs="?", type=int, const=60, metavar="SECS",
                   help="nudge unconditionally for SECS seconds (default 60), bypassing playback "
                        "detection — to test whether the nudge still keeps the screen on")
    args = p.parse_args(argv)

    interval = resolve_interval(args.interval)
    if args.check:
        return cmd_check()
    if args.force is not None:
        return cmd_force(args.force, interval)
    run_daemon(interval)
    return 0


if __name__ == "__main__":
    sys.exit(main())
