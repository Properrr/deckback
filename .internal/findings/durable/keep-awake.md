---
topic: steamdeck-keep-awake
status: SOLVED (host-side uinput helper) — verified on-Deck (OLED) 2026-07-11. Regressed 2026-07-14
  when the helper SELF-UNINSTALLED during a routine deploy (uninstall→install) and sat disabled;
  root-caused, fixed, redeployed and RE-VERIFIED on-Deck 2026-07-23 (screen stayed lit past the
  timer during playback). Not a SteamOS change; the truncation hypothesis was refuted on hardware.
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

## 2026-07-23 regression: screen dims + sleeps mid-playback again (detection hardened)

A user reported the screen dimming on a timer and then suspending **during playback** — the exact
symptom this helper exists to prevent — after what looks like a SteamOS update. Root-cause candidate
(strong, and the one fixed): the helper decided "is a video playing?" by grepping the **human-readable**
`systemd-inhibit --list` table for the substring `"playback active"`. That table **ellipsizes its WHY
column to the terminal width**, and our marker sits at the tail of a long WHY (`Deckback: playback
active`). SteamOS ships its own systemd; a version bump that tightens truncation (or changes the
no-tty width) drops the `active` and the grep silently returns false → the helper stops nudging →
gamescope's idle timer fires → dim, then suspend. No error anywhere: "nothing is playing" and
"detection broke" are indistinguishable from outside.

**Fix (`scripts/idle-nudge.py`, 2026-07-23):** detection now reads logind's `ListInhibitors` **over
D-Bus** (`busctl --json=short call org.freedesktop.login1 … ListInhibitors`), whose reply carries the
full untruncated WHY for every inhibitor. Verified on the workstation that the reply is `a(ssssuu)`
JSON with WHY verbatim (so the substring match holds) and that `busctl` is present (ships with
systemd, so it is on the Deck too). Falls back to the old text table (with `COLUMNS=1000` forced) only
if `busctl` is unavailable. L0 regression test: `tests/harness/test_idle_nudge.py` asserts the matcher
is True on the busctl/wide-table source and — freezing the bug — False on the truncated table.

**Diagnosability added** (because this failed *silently* for the user): the daemon now logs each
playback→idle transition and the detection source to the journal, and there are two on-Deck probes:
- `deckback-idle-nudge --check` — prints whether the playback inhibitor is detected right now + the
  raw listing. Separates "detection broke" from everything else.
- `deckback-idle-nudge --force <secs>` — nudges unconditionally with detection bypassed, so you can
  start a video and watch whether the **nudge itself** still holds the screen on. If it dims even
  under `--force`, the regression is NOT detection — a newer gamescope has stopped honoring the
  synthetic uinput device for its idle timer, and the *nudge mechanism* needs to change (candidates,
  none yet tried on the new SteamOS: larger real displacement, a genuine BTN tap despite the
  page-leak risk, or a new gamescope idle-reset API). Record which branch the Deck actually hits.

### ROOT CAUSE — found on-Deck 2026-07-23. It was NOT the truncation, and NOT SteamOS.

Diagnosed on the OLED unit (SteamOS BUILD_ID 20260716.1, systemd 257):

- `deckback-idle-nudge.service` was **`disabled` + `inactive (dead)`**, while `deckback-audio-repair`
  was active. The unit file and binary were still present, dated at their install.
- The `default.target.wants/` symlink was gone, dir mtime **Jul 14 16:41** — i.e. it was disabled
  then, **two days BEFORE** the Jul 16 SteamOS update. The screen has dimmed ever since simply
  because the helper was not running.
- **The helper self-uninstalled.** `flatpak history` shows the deploy cycle is an *uninstall*
  immediately followed by an install (9 such pairs across Jul 16–23) — `just deck-install-dev`. The
  watcher saw the app dir vanish, hit its 3-poll (~75 s) debounce, and ran `disable --now`. The
  `disable` succeeded (symlink gone); the `rm` never ran, because of the cgroup bug below — which is
  exactly why the files were left behind and it still *looked* installed.

**The truncation hypothesis is REFUTED.** On this Deck `systemd-inhibit --list | grep "playback
active"` matches fine — the WHY column is not truncated under systemd 257. The old detection would
have worked. The switch to `busctl ListInhibitors` is still worth keeping as hardening (structured
data beats scraping a human-readable table), but it fixed nothing here, and the earlier commit
message calling it the likely root cause was wrong.

**The actual fix** is the self-removal grace period: it now requires **15 minutes of continuous
absence** (`MISSING_GRACE_SECONDS`, wall-clock, not poll ticks) in both helpers. Deploy gaps measured
9–15 s. The helper is inert without Deckback anyway — it only nudges while the playback inhibitor
exists — so waiting costs nothing, and self-removing mid-deploy costs the whole feature. The cgroup
fix (`systemd-run --user`) stays: without it self-removal leaves a half-cleaned system, which is what
disguised this for nine days.

**Verified on-Deck 2026-07-23** (all through the real device): service enabled + active and surviving
reboot (WantedBy symlink restored), the new journal logging prints `playback active — nudging`,
`--check` returns True via the `busctl` source against the live `["idle","Deckback","Deckback:
playback active","block"]` inhibitor, and the `deckback-idle-nudge` uinput device is present in
`/proc/bus/input/devices`. **The panel was then watched by the user during playback and stayed lit
past the configured Steam timer (the 1 min / 5 min setting) with no suspend** — the same gate the
2026-07-11 result used, so the regression is closed end to end.

**Re-confirmed the same day through a full deploy**, which is the strongest form of this test: a new
bundle was built from main and installed with `just deck-install-dev`, i.e. the exact
flatpak uninstall→install cycle that caused the original self-removal. The helper came through it
with `NRestarts=0`, continuously active across the deploy, and never even entered the grace period
(no poll landed in the ~10 s uninstall gap). The user then confirmed the screen still does not dim.
So the fix holds against the real trigger, not just against the symptom.

Also fixed: `install-audio-repair.sh` used `enable --now`, which does not restart an already-running
unit, so reinstalling it never picked up new code (observed on-Deck: the pre-update process was still
live after a reinstall). It now `restart`s, like the idle-nudge installer already did.

## The helper is invisible if it is not installed — so the app now says so (2026-07-23)

This whole mechanism is opt-in and lives OUTSIDE the app, so "user never ran `just idle-nudge`" and
"the helper broke" produce the identical symptom. Nothing in-app noticed either. The launcher now asks
the host user systemd (`org.freedesktop.systemd1`, new `--talk-name` in the manifest) for
`deckback-idle-nudge.service`'s ActiveState — once, on the first poll tick that observes playback —
and on `Inactive` logs a warning and shows a one-time toast. `Unknown` (bus unreachable, name not
permitted) stays **silent**: warning about something we cannot see would be unactionable noise every
launch. `power.keep_awake_warn: false` disables it. Tests: `player_test` covers warn-once, silent on
Active, silent on Unknown, and never probed while idle.

It can only REPORT. A sandboxed Flatpak cannot install a host systemd unit — no host exec, no write
access to `~/.config/systemd/user` — so "detect and auto-install" is not achievable, and install stays
a Desktop-Mode step. Do not re-attempt the auto-install.

## Uninstall leftovers (2026-07-23)

Flatpak has no onUpgrade/onRemove hook that runs host commands (that is the whole reason the helpers
poll for the app dir disappearing). Two gaps closed alongside the above:

- **audio-repair never self-cleaned** — its unit + binary stayed enabled and running forever after
  Deckback was removed. It now runs the same debounced watcher (app dir gone ~30 s → self-remove).
- **`scripts/uninstall.sh` / `just uninstall`** removes app + both helpers + the Steam shortcut and
  grid art + per-app overrides in one step. App data (`~/.var/app/<id>`, the YouTube sign-in) is KEPT
  by default so a reinstall resumes; `--purge` wipes it.

Also fixed a latent bug in the pre-existing self-uninstall: it detached the cleanup with `setsid`, but
setsid does NOT leave the unit's cgroup, so systemd's default `KillMode=control-group` SIGTERMs the
cleanup during its own `disable --now` — usually before the `rm`, leaving the unit and binary behind.
Both helpers now run the cleanup as a transient unit (`systemd-run --user --collect`), i.e. its own
cgroup. Neither self-clean path is hardware-verified.
