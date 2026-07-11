---
topic: steamdeck-keep-awake
status: SOLVED (host-side uinput helper) — verified on-Deck (OLED) 2026-07-11
---

# Keeping the Deck awake during playback (screen-off / auto-suspend)

SteamOS Game Mode dims the screen and auto-suspends after a few minutes with no **controller** input;
video playback does **not** count as activity to it. Everything below is verified on-Deck (OLED)
2026-07-11. The shortest sleep timer selectable in Steam settings is **1 minute**.

## What does NOT work — dead ends, do not re-attempt

Nothing reachable from inside the Flatpak sandbox can inhibit gamescope/Steam's input-idle timer:

- **logind `idle`-block inhibitor** — held during playback, but does NOT stop the screen blanking or
  the suspend (the Deck's display-off and suspend are driven by Steam/gamescope, not logind's idle
  action). We keep it anyway, purely as the `"playback active"` signal the helper gates on.
- **logind `sleep`-block inhibitor** — DOES stop the suspend, but it blocks a *deliberate*
  power-button sleep too (bad), and never stops the SCREEN blanking. Shipped briefly 2026-07-11,
  then retired in the same session.
- **`org.freedesktop.ScreenSaver`** — no such service in Game Mode (the manifest `--talk-name` is dead).
- **XDG `org.freedesktop.portal.Inhibit`** — interface not implemented by gamescope's portal backend.
- **`/dev/uinput` from the sandbox** — absent (`--device=input` grants `/dev/input/*`, not uinput).
- **libei / gamescope EIS (`gamescope-0-ei`)** — the sandbox CAN reach it (bind `xdg-run/gamescope-0-ei`,
  connect directly — gamescope's EIS accepts a raw connection, no portal — and injection works). BUT
  **gamescope ignores emulated input for its idle timer.** Proven: a libei pointer nudge every 25s did
  NOT keep the screen on; the idle fired **4 s after a fresh nudge**. Emulated input is deliberately
  not idle-defeating. `nudge/nudge.c` + the libei flatpak module were built and then removed.
- **Native Wayland idle-inhibit** — gamescope's Wayland globals do NOT advertise
  `zwp_idle_inhibit_manager_v1` (checked with `wayland-info` on `gamescope-0`), so even porting the
  engine to native Wayland would not let Chromium's own video idle-inhibitor keep the screen on.
- **VacuumTube** (sibling Electron Leanback client) hit the identical wall: no `powerSaveBlocker`,
  only DBus `org.*.ScreenSaver` talk-names that gamescope ignores; the maintainer called it a Steam
  Game Mode bug and left issue #67 open. Independent confirmation that in-process inhibits are dead
  under gamescope.

## What DOES work: a host-side REAL-input nudge

gamescope/Steam's idle timer counts **real evdev devices**, not emulated input. So a host-side helper
— OUTSIDE the sandbox, where `/dev/uinput` is writable (uaccess ACL for the seat user) — creates a
uinput pointer and nudges it every ~25 s (< the 1-min minimum sleep timer). The timer then never
fires: **screen stays on AND no auto-suspend**, and because it *resets* the timer rather than
*blocking* suspend, the power button still works.

- `scripts/idle-nudge.py` + `scripts/deckback-idle-nudge.service` + `scripts/install-idle-nudge.sh`
  (`just idle-nudge`), mirroring the `audio-repair` host helper.
- **Gated on the launcher's `"playback active"` (idle-block) inhibitor**, so it nudges only during
  playback — the Deck still sleeps in menus / when paused.
- The nudge is a **pointer** motion (not a key): `config/no_pointer.js` swallows it in the page, so it
  never surfaces YouTube's player controls. A key would leak through.
- **Self-uninstalls** when Deckback is removed (Flatpak can't run host commands on uninstall, so the
  helper watches for the app dir disappearing and removes its own unit + binary).
- Verified on-Deck 2026-07-11: screen stayed lit **110 s past a 60 s timer, 0 suspends**; a deliberate
  power-button press sleeps normally during playback.

The app itself now holds only the harmless `idle`-block (the play-state signal) plus the `sleep`-DELAY
(checkpoint-before-sleep) inhibitor — **no `sleep`-block** — so a deliberate sleep always works, and
suspend-prevention is entirely the host helper's job.
