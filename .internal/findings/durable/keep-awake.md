---
topic: steamdeck-keep-awake
status: SOLVED (host-side uinput helper) — verified on-Deck (OLED) 2026-07-11, re-verified through a
  full deploy cycle 2026-07-23 (screen stayed lit past the Steam timer during playback).
---

# Keeping the Deck awake during playback (screen-off / auto-suspend)

SteamOS Game Mode dims the screen and auto-suspends after a few minutes with no **controller** input;
video playback does **not** count as activity to it. The shortest sleep timer selectable in Steam
settings is **1 minute**. Everything below is verified on-Deck (OLED) unless marked otherwise.

## What does NOT work — dead ends, do not re-attempt

Nothing reachable from inside the Flatpak sandbox can inhibit gamescope/Steam's input-idle timer:

- **logind `idle`-block inhibitor** — held during playback, but does NOT stop the screen blanking or
  the suspend (display-off and suspend are driven by Steam/gamescope, not logind's idle action). We
  keep it anyway, purely as the `"playback active"` signal the helper gates on.
- **logind `sleep`-block inhibitor** — DOES stop the suspend, but also blocks a *deliberate*
  power-button sleep, and never stops the screen blanking. Not shipped.
- **`org.freedesktop.ScreenSaver`** — no such service in Game Mode.
- **XDG `org.freedesktop.portal.Inhibit`** — not implemented by gamescope's portal backend.
- **`/dev/uinput` from the sandbox** — absent (`--device=input` grants `/dev/input/*`, not uinput).
- **libei / gamescope EIS (`gamescope-0-ei`)** — reachable from the sandbox (bind
  `xdg-run/gamescope-0-ei`, connect directly; no portal needed) and injection works, but **gamescope
  ignores emulated input for its idle timer**: a libei pointer nudge every 25 s did not keep the
  screen on, and the idle fired 4 s after a fresh nudge.
- **Native Wayland idle-inhibit** — gamescope does not advertise `zwp_idle_inhibit_manager_v1`
  (`wayland-info` on `gamescope-0`), so even a native-Wayland engine could not use Chromium's own
  video idle-inhibitor.
- **VacuumTube** (sibling Electron Leanback client) hit the same wall: its DBus ScreenSaver
  talk-names are ignored by gamescope, and issue #67 stays open.

## What works: a host-side REAL-input nudge

gamescope/Steam's idle timer counts **real evdev devices**. A host-side helper — outside the sandbox,
where `/dev/uinput` is writable (uaccess ACL for the seat user) — creates a uinput pointer and nudges
it every 25 s (`DEFAULT_INTERVAL`, clamped 5–55 s, always under the 1-minute minimum timer). The
timer never fires: **screen stays on and no auto-suspend**. Because it *resets* the timer rather than
*blocking* suspend, the power button still sleeps the Deck.

- `scripts/idle-nudge.py` + `scripts/deckback-idle-nudge.service` + `scripts/install-idle-nudge.sh`
  (`just idle-nudge`), alongside the `audio-repair` host helper.
- **Gated on the launcher's `"playback active"` idle-block inhibitor**, so it nudges only during
  playback — the Deck still sleeps in menus and when paused.
- **Detection** reads logind's `ListInhibitors` over D-Bus
  (`busctl --json=short call org.freedesktop.login1 … ListInhibitors`), whose reply carries the full
  WHY string. Fallback when `busctl` is absent: the `systemd-inhibit --list` table with
  `COLUMNS=1000`. (On SteamOS's systemd 257 that table is not truncated either.) L0:
  `tests/harness/test_idle_nudge.py`.
- The nudge is a **pointer** motion, not a key: `config/no_pointer.js` swallows it in the page, so it
  never surfaces YouTube's player controls. A key would leak through.
- The app itself holds only the `idle`-block (play-state signal) and the `sleep`-**delay**
  (checkpoint-before-sleep) inhibitor — no `sleep`-block — so a deliberate sleep always works.

Verified on-Deck: the screen stayed lit 110 s past a 60 s timer with 0 suspends, and a power-button
press sleeps normally during playback (2026-07-11). Re-verified 2026-07-23 (SteamOS BUILD_ID
20260716.1, systemd 257): service enabled and surviving reboot, journal shows
`playback active — nudging`, `--check` detects the live inhibitor via `busctl`, the
`deckback-idle-nudge` device is in `/proc/bus/input/devices`, and the panel stayed lit past the Steam
timer during playback — including straight after a `just deck-install-dev` (flatpak uninstall →
install) cycle, with the helper continuously active (`NRestarts=0`).

### Diagnostics

- The daemon logs each playback↔idle transition and its detection source to the journal.
- `deckback-idle-nudge --check` — prints whether the playback inhibitor is detected now, plus the
  raw listing. Separates "detection broke" from everything else.
- `deckback-idle-nudge --force [SECS]` — nudges unconditionally (default 60 s). If the screen still
  dims under `--force`, the nudge mechanism itself has stopped working (e.g. a gamescope change), not
  detection.

## Self-removal

Flatpak has no hook that runs host commands on uninstall, so both host helpers (idle-nudge and
audio-repair) watch for the app dir to disappear and then remove their own unit and binary.

- **Grace period: 15 minutes of continuous absence** (`MISSING_GRACE_SECONDS = 900`, wall-clock) in
  both helpers. A deploy is an uninstall immediately followed by an install, with gaps measured at
  9–15 s; a shorter debounce disables the helper mid-deploy. The helper is inert without Deckback
  anyway, so waiting costs nothing.
- The cleanup runs as a transient unit (`systemd-run --user --collect`) so it leaves the service's
  cgroup. A `setsid`-detached cleanup is still in the cgroup and gets SIGTERMed by the service's own
  `disable --now` (`KillMode=control-group`) before its `rm`, leaving a disabled-but-present unit.
- `install-audio-repair.sh` and `install-idle-nudge.sh` both `restart` the unit, so a reinstall picks
  up new code (`enable --now` does not restart a running unit).
- **Not hardware-verified:** either helper's self-clean path actually running to completion.

## The app warns when the helper is not running

The helper is opt-in and lives outside the app, so "never installed" and "broken" look identical.
The launcher asks the host user systemd (`org.freedesktop.systemd1`, `--talk-name` in the manifest)
for `deckback-idle-nudge.service`'s ActiveState — once, on the first poll tick that observes
playback. `Inactive` → a warning and a one-time toast; `Unknown` (bus unreachable, name not
permitted) → silent, since a warning about something we cannot see is unactionable.
`power.keep_awake_warn: false` disables it. `player_test` covers warn-once, silent on Active, silent
on Unknown, and never probed while idle.

It can only report. A sandboxed Flatpak cannot install a host systemd unit (no host exec, no write
access to `~/.config/systemd/user`), so install stays a Desktop-Mode step. Do not attempt
auto-install.

## Uninstall

`scripts/uninstall.sh` / `just uninstall` removes the app, both helpers, the Steam shortcut and grid
art, and per-app overrides. App data (`~/.var/app/<id>`, the YouTube sign-in) is kept by default so a
reinstall resumes; `--purge` wipes it.
