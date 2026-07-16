# Deckback Configurator — design doc

> **SUPERSEDED 2026-07-15 by `.internal/osd-menu-plan.md` (user decision).** The separate "Deckback
> Settings" companion app is not being built; its role is absorbed by the **in-app OSD Settings
> menu**. The load-bearing decisions here — the sparse `user.json` overlay (C2), the rule that user
> config must not shadow the shipped R1 hotfix keys (`url`/`user_agent`/`cobalt_flags`/`cdm_*`/
> `remote_debugging_port`), button-remap conflict/lockout protection, no per-direction-D-pad / no
> rear-grip remap, the warned hardware-decode toggle, and reset-to-defaults — are migrated into the
> OSD plan's writable-settings roadmap (osd-menu-plan.md §11, O10). This document is kept for history
> and rationale only; do not implement from it.

Status: **DESIGN — SUPERSEDED (do not implement).** Companion to
`.internal/steamdeck-cobalt-youtube-plan.md` (the main plan wins on conflict for everything about the
YT app itself; this doc owned the configurator). Written 2026-07-10 against launcher/config state at
commit `1af3e6b` and `config/app.json` schema_version 1.

## 1. What and why

A second bundled app, **"Deckback Settings"**, launchable from the Steam library alongside the
main YT client. It is a gamepad-first (plus touch) UI that edits every user-facing config key,
grouped into tabs, with per-key and global **Reset to defaults**. Headline features requested:

- enable/disable **hardware decode** (VA-API) — as an *experimental, warned* toggle (§6),
- enable/disable the **touchscreen** default (`disable_touch`),
- **remap any Deck button** to any YT action (fast-forward, rewind, pause, play, play/pause,
  navigation up/down/left/right, etc.) with conflict/lockout protection,
- multiple tabs, simple UX, reset to defaults.

Why now: today the only way to change behavior is editing `/app/share/deckback/app.json` — which
is **read-only inside the Flatpak**, so in practice users cannot change anything at all without a
rebuild. The configurator is also the delivery vehicle for the first *user-writable* config layer,
which the main plan needs anyway (R7 "hotfixes via config channel").

Branding: never "YouTube"/"YT" in id or name. App name **"Deckback Settings"**, desktop id
`io.github.properrr.deckback.Settings` (same Flatpak, second exported desktop file — §8).

## 2. Non-goals (v1)

- No live reconfiguration of a running YT app. Config is read once at launcher startup
  (main.cpp loads it exactly once); the configurator edits on disk and the change applies on next
  launch of the main app. The UI says so explicitly after saving.
- No editing of `url`, `user_agent`, `cobalt_flags`, `cdm_url`/`cdm_sha256`, or
  `remote_debugging_port`. These are the R1 hot-swap/security surface and stay owned by the
  *shipped* app.json (and the future P10 signed remote channel). Exposing them to users would let
  the user config shadow an emergency UA hotfix — see the overlay rule in §5.
- No per-direction dpad remap (`dpad` is resolved on the axis path as a unit; v1 keeps it
  `navigate`-only) and no rear-grip (L4/L5/R4/R5) remap — grips are duplicated to face buttons by
  Steam Input (`steam_input.vdf`), invisible to the launcher's evdev layer.
- No desktop-mode-styled windowing work; target is Game Mode at 1280×800, same as the main app.
- Voice settings stay hidden until voice is hardware-verified (TEST-PLAN §2 lists mic capture and
  voice search as never verified). Shipping a knob for an unverified feature manufactures support
  load.

## 3. Locked decisions

| # | Decision | Rationale |
|---|----------|-----------|
| C1 | Configurator is the **same `deckback-launcher` binary** in a `--configurator` mode, rendering an **embedded HTML/JS app in the existing Cobalt `content_shell` over CDP** | Zero new dependencies (repo rule: libsystemd/libevdev/libcurl only). The CDP UI stack is already proven on-Deck: `errorpage.cpp`/`overlay.cpp`/`onboarding.cpp` build interactive, focusable DOM UI via injection; evdev→CDP input already works; gamescope treats it exactly like the main app. SDL/ImGui or GTK/Qt would add deps, a second input stack, and a second look-and-feel for no benefit. Startup cost (~2–4 s engine boot) is acceptable for a settings app. |
| C2 | User settings live in a **sparse overlay file** `$XDG_CONFIG_HOME/deckback/user.json` containing *only keys the user changed*; the launcher loads shipped `app.json` first, then applies the overlay | Shipped defaults (especially UA and `cobalt_flags`) must keep flowing through updates — a full-copy user config would freeze the R1 lifeline at fork time. Sparse overlay makes "Reset to defaults" = delete the file, and per-key reset = drop the key. |
| C3 | Overlay applies to an **allowlist** of user-safe keys only (§5); anything else in user.json is ignored and logged | Defense for R1/R7 and for future signed remote config: the remote channel and the user channel must not fight over the same keys. |
| C4 | HW decode is exposed as a first-class boolean `hw_decode` (default `false`), which the launcher expands to `--enable-features=VaapiVideoDecodeLinuxGL --use-angle=gl` at engine-arg composition time — **not** by letting users edit `cobalt_flags` | The exact two-flag recipe is a milestone fact (m114.md) that will change on Cobalt bumps; a semantic key survives bumps, a raw-flag edit doesn't. Also keeps the corrupt-decode warning attached to one switch. |
| C5 | Configurator uses a **fixed built-in keymap** (dpad=focus move, A=select, B=back, X=per-key reset, Start=save/quit menu), *never* the user's keymap | The user may be mid-edit of a broken keymap; the tool that repairs the keymap must not depend on it. |
| C6 | Same single-instance lock (`$XDG_RUNTIME_DIR/deckback.lock`) as the main app | Both fork `content_shell` and default to port 9222; concurrent run is useless anyway (main app won't see edits until relaunch). On contention the configurator prints/​shows "Deckback is running — close it first" and exits 3 (environment, per HARNESS taxonomy). |
| C7 | Touch is **enabled** in the configurator | It skips the `no_pointer.js` injection and does not hold `STEAM_TOUCH_CLICK_MODE` at hover. A settings app is exactly where taps help, and it demonstrates the touch machinery is a per-app policy, not a global kill. |

## 4. Architecture

```
Steam library
 ├─ "Deckback"            → deckback.sh          → deckback-launcher --config …app.json
 └─ "Deckback Settings"   → deckback-settings.sh → deckback-launcher --config …app.json --configurator
```

`--configurator` branches in the `main.cpp` arg loop (same pattern as the existing
`--selftest-dbus` short-circuit mode, but *after* the single-instance lock — see C6) into
`run_configurator(cfg_path)`:

1. **Load** shipped config + user overlay (new `ConfigStore` module, §5) → three JSON documents:
   `defaults` (compile-time `Config` defaults), `shipped` (app.json as parsed), `user` (overlay).
2. **Boot engine** exactly like the main flow (watchdog-forked `content_shell` on `about:blank`),
   but with: an **ephemeral profile dir** (`$XDG_STATE_HOME/deckback/configurator-profile`,
   wiped at startup — no cookies/sign-in needed, and it must never touch the real profile),
   the same debugging port, no AV1/no-pointer injection.
3. **Inject UI**: a single self-contained HTML/CSS/JS app, embedded at build time via the existing
   CMake `*.js → *_js.hpp` pattern (`config/configurator_app.js` + inline CSS; same rationale as
   `no_pointer.js` — a missing file must not silently ship a broken settings app). Injected into
   `about:blank` via the errorpage/overlay technique (no Trusted Types problem off youtube.com).
4. **Bridge** (launcher ⇄ page, both over the existing CDP client):
   - *Down:* launcher evals `window.__deckback_init(payload)` with
     `{schema, defaults, shipped, user, effective, meta}` — `meta` carries app version and
     per-key annotations (which keys are experimental, which need the warning).
   - *Up:* the page pushes commands onto `window.__deckback_ops` (array of
     `{op: "set"|"reset"|"reset_all"|"save"|"quit"|"launch_main", key?, value?}`); the launcher
     polls and drains it every 250 ms — same poll-over-eval pattern `player.cpp` already uses at
     1 Hz. No new protocol machinery.
   - *Input:* the existing evdev reader runs with the fixed C5 keymap; buttons become
     `Input.dispatchKeyEvent` arrows/Enter/Escape, which the page handles as ordinary `keydown`.
     A small extension: in **capture mode** (§7) the launcher suspends keymap resolution and
     instead evals `window.__deckback_raw_button("<control>")` per press.
5. **Writes** go through `ConfigStore::save()` — atomic (`tmp` + `fsync` + `rename`) into
   `$XDG_CONFIG_HOME/deckback/user.json`. In the Flatpak that resolves to the durable
   `~/.var/app/io.github.properrr.deckback/config/deckback/user.json`.
6. `launch_main` op: configurator releases the lock, then `exec`s the main flow (drops
   `--configurator` from its own argv) so "Save & launch Deckback" is one button.

New launcher files: `configurator.{cpp,hpp}` (mode driver + ops loop), `config_store.{cpp,hpp}`
(overlay load/merge/serialize/atomic-write; the first *writer* this codebase has). `config.cpp`
grows one function: `Config::apply_overlay(path, allowlist)` reusing the existing extractors with
set-only-if-present semantics (they already only assign on key hit; the work is plumbing a second
pass, not rewriting the parser).

Main-app change: `main.cpp` calls `apply_overlay()` right after `Config::load()`. That is the
*entire* footprint on the YT app path — deliberately tiny, so the configurator cannot destabilize
the thing it configures.

## 5. Config model: three layers, one allowlist

Precedence (lowest→highest): compile-time defaults → shipped `app.json` → **user overlay** →
`DECKBACK_EXTRA_ARGS`/env (unchanged, debug-only). The future P10 signed remote config slots in
between shipped and user *for its own key set*; the allowlist keeps the channels disjoint.

**User-editable allowlist (v1):**

| Tab | Keys |
|-----|------|
| Playback | `hw_decode` (new), `max_height` (720/1080), `steer_av1` |
| Controls | `keymap` (whole-object replace), `right_stick_scroll`, `right_stick_deadzone`, `right_stick_slow_ms`, `right_stick_fast_ms` |
| Touch & System | `disable_touch`, `first_run_overlay`, `restart_on_crash`, `error_page` |
| (hidden until verified) | `voice_enabled`, `voice_hold_ms`, `voice_duck`, `voice_click_toggles` |

Everything else (`user_agent`, `url`, `cobalt_flags`, CDM keys, log/power internals, dead
`touch_lock_*`) is **not** overlayable and not shown. `user.json` carries `schema_version`; on a
major-version mismatch the launcher ignores the overlay, logs it, and the configurator offers
"reset all" — never guess-migrate silently.

**Reset semantics:** per-key reset removes the key from `user.json` (value falls back to shipped);
"Reset all to defaults" (with confirm dialog) deletes `user.json`. Because the overlay is sparse,
"default" always means *current shipped default*, not a stale snapshot.

## 6. The HW decode toggle — handle with care

This is the one setting that is **known-broken upstream**: on-Deck 2026-07-10 the VA-API path
produced green-band corruption on every frame while every automated metric passed
(m114.md §"VA-API decode is VISUALLY CORRUPT"; CLAUDE.md forbids re-adding the flags to the
*shipped* config without a pixel gate). The configurator changes the situation: the flags are no
longer shipped-on, they are **user-opt-in**, which is legitimate — future SteamOS/Mesa updates may
fix the radeonsi DMA-buf import, and users need a way to try without a rebuild. Requirements:

- Lives under Playback as **"Hardware video decode (experimental)"**, default off.
- Flipping it on interposes a full-screen warning: *"Known to draw green corrupted video on
  current SteamOS. If video looks wrong, turn this back off. Battery savings are unverified."*
  Requires an explicit second confirm.
- The main app, when `hw_decode=true`, shows a one-time toast on first playback ("Hardware decode
  is ON — if video looks corrupted, open Deckback Settings") via the existing overlay primitive,
  so a user who forgot the toggle can find their way back.
- The CI pixel gate (`b3fcd58`) stays authoritative for ever flipping the *shipped* default.
- Do **not** generalize to "decoder selection" UI; it's one boolean expanding to the m114 flag
  pair. On a Cobalt bump, MIGRATION.md gains a line: re-verify the `hw_decode` flag expansion.

## 7. Key remapping

**Data model** — unchanged from today: `keymap` is control→value where value is a semantic action
or a literal DOM key (literal wins; that stays the hot-swap lever). The configurator edits this
object and writes it whole into the overlay.

**Action catalog** shown in the picker (grounded in `input.cpp` `kActionAliases` + plan §P3):

| Action | DOM key | Status |
|--------|---------|--------|
| Select / OK | `Enter` | verified on-Deck |
| Back | `Escape` | verified |
| Play/Pause | `MediaPlayPause` | verified |
| Fast-forward (scrub) | `ArrowRight` (player context) | verified |
| Rewind (scrub) | `ArrowLeft` | verified |
| Toggle captions | `c` | verified |
| Navigate Up / Down / Left / Right | `ArrowUp/Down/Left/Right` | verified (dpad path) |
| Play (discrete) | `MediaPlay` | **unverified on Leanback** |
| Pause (discrete) | `MediaPause` | **unverified** |
| Scan rewind / forward, Voice search, Player menu | — | no known key; **not offered** until an on-Deck spike lands one (no-guessing policy, plan §P3) |

Unverified actions appear in the picker greyed with "(may do nothing)" — or are cut from v1 if we
want zero support noise; decide at implementation time after a 10-minute on-Deck spike of
`MediaPlay`/`MediaPause`, which is cheap and settles the row. An **Advanced: raw key…** entry lets
power users type any DOM key string (that's just exposing the existing literal-key semantics).

**UX** — the Controls tab is a list of physical controls (A, B, X, Y, LB, RB, LT, RT, Start,
Select, L3, R3; dpad shown fixed as "Navigate"). Two paths to rebind:
- *Pick*: focus a row, press A → action picker dialog.
- *Capture* ("Find button"): from the picker, "press the button instead" — launcher enters capture
  mode (C5/§4), the next physical press selects the row for that control. Good for "which one is
  LB again" moments.

**Lockout protection** (the configurator must never brick navigation in the YT app): `select` and
`back` must each be bound on at least one *base-layer* button at save time; a save violating this
is blocked with an inline explanation. Duplicate assignments are allowed (two buttons → same
action) but flagged with a subtle badge. A live **input tester** strip at the bottom of the tab
shows `last pressed: RB → scrub_fwd` using events the launcher already sees — free confidence, no
extra machinery.

**Layers** (`keymap_player`, `keymap_osk`, `keymap_lt`, `keymap_rt`) ship empty today and stay
out of the v1 UI; the overlay format already carries them, so a later "Pro" sub-tab is additive.

## 8. Packaging & Steam integration

- Flatpak: same app, second exported desktop file `io.github.properrr.deckback.Settings.desktop`
  (`Exec=deckback --configurator`, `Categories=Settings;`, own icon — gear-badged variant of the
  main icon). Flatpak exports any desktop file prefixed with the app id; the `pack` stage
  (debian:13, flatpak-builder ≥1.4) handles multiple desktop files fine. Metainfo keeps a single
  `<launchable>`; run `just flatpak-lint` — if Flathub QA objects to the second desktop file
  (known lint `desktop-file-not-installed` class of complaints), fallback is documented below.
  The wrapper `deckback.sh` already forwards `"$@"`, so a one-line
  `deckback-settings.sh` (`exec /app/bin/deckback --configurator "$@"`) is all the glue.
- Steam/Game Mode: users add each desktop entry as a non-Steam game (both appear in Desktop Mode
  app grid; SUPPORT.md gets a two-screenshot section). There is no way for one non-Steam shortcut
  to "contain" two apps — two library entries is the native pattern.
- **Fallback / complement — in-app entry:** holding **Start for 3 s** in the YT app quits to the
  configurator (launcher-side chord → exec self with `--configurator`). This works even if the
  user only added one shortcut, and is the escape hatch if Flathub rejects the second desktop
  file. The chord is launcher-owned (not keymap-resolvable), so a remap can't disable it —
  it is the recovery path of last resort and must be unconditional.
- Exit-code taxonomy (HARNESS.md) applies: configurator exits 0 on save/quit, 2 if the UI failed
  to boot/inject (product wrong), 3 on lock contention or missing engine (environment).

## 9. UI / IA

Four tabs, LB/RB (or ←/→ on a focused tab bar) to switch — LB/RB are safe here because the
configurator keymap is fixed (C5):

1. **Playback** — Hardware decode (experimental, §6) · Max resolution (720p/1080p) · "Prefer
   VP9/H.264 over AV1" (on, with one-line why).
2. **Controls** — remap list + input tester (§7) · right-stick scrolling toggle + speed
   (Slow/Normal/Fast presets mapping onto the three `right_stick_*` numbers; raw numbers only
   under Advanced).
3. **System** — Touchscreen enabled in app (inverts `disable_touch` for display — the UI says
   "Touchscreen" ON/OFF, not "disable_touch" false/true) · Show controls guide on next launch
   (`first_run_overlay`, doubles as "how do I see that card again") · Restart after crash ·
   Friendly error page · **Reset all settings**.
4. **About** — version, DEPS.pin milestone, links (SUPPORT.md/legal), config file path for the
   curious, and the honest line "Unofficial app; not affiliated with YouTube".

Style: 10-foot UI, dark, big focus ring, one settings column, description text under the focused
row (Leanback-like without imitating YouTube trade dress). Every changed-from-default row gets a
dot badge + X-to-reset. Footer shows persistent button hints (`A Select · B Back · X Reset ·
Start Save`). Save model: **explicit** — Start opens Save/Discard/Keep-editing; B from the tab
bar with unsaved changes prompts. Explicit save beats instant-apply here because settings take
effect on next launch anyway, and it gives "Discard" for free.

## 10. Test plan (per TEST-PLAN §0 — failing on-device test first)

- **L0 (`tests/harness/` + launcher unit tests):** `ConfigStore` overlay merge (user overrides
  shipped; non-allowlisted key ignored+logged; schema mismatch → overlay dropped), writer
  round-trip + atomicity (kill between tmp-write and rename leaves old file intact), keymap
  lockout validator (a save without `select`/`back` must FAIL — a check that cannot fail is not
  a check), `hw_decode` → exact m114 flag-pair expansion, and `reset_all` → file gone.
- **L1 (`just smoke` extension):** Xvfb boot of `--configurator`; CDP asserts the tab bar
  rendered, drives one `set`+`save` through `__deckback_ops`, restarts the *main* flow headless
  and asserts the overlay value is live in the composed engine args. Screenshot artifact.
- **L2 (`just test-deck`):** on-Deck: launch Settings from Game Mode; remap X→captions; save;
  launch main app; verify X now toggles captions and the persisted `user.json` matches. Touch a
  toggle with a finger (C7 — the only place touch is *supposed* to work). Toggle `hw_decode` on,
  confirm warning flow, launch main, run the **pixel gate** against playback, toggle back off.
- **Human:** 10-foot readability at 800p on the OLED unit; LCD unit still has run nothing —
  applies here too, say so in any status claim.

## 11. Risks

| Risk | Impact | Mitigation |
|------|--------|-----------|
| Flathub rejects second desktop file | No library entry for Settings | Start-hold chord (§8) is the guaranteed entry; ship it regardless |
| Users enable `hw_decode`, see corruption, file bugs | Support noise for a known issue | Double confirm + in-app toast pointing back to Settings (§6); SUPPORT.md FAQ entry |
| Hand-rolled parser meets nested user edits | Silent misparse | Overlay is machine-written by us, flat keys, allowlisted; humans hand-editing `user.json` is unsupported-but-tolerated (parse errors → overlay ignored + logged, never a crash) |
| Overlay shadows a future emergency hotfix | R1 response blunted | Allowlist (C3) keeps UA/url/flags un-overlayable — this is why C2/C3 are locked |
| Broken keymap saved | User can't navigate YT app | Lockout validator (§7) + unconditional Start-hold chord back into Settings + Reset all |
| Engine boot too slow to feel like a "settings app" | UX | Acceptable for v1; measure on-Deck; splash text injected pre-init if >2 s |

## 12. Phasing

- **CFG-1:** `ConfigStore` (overlay read/merge in main app + writer) + `hw_decode` key + L0
  tests. *Ships value alone: power users get a supported user.json even before any UI.*
- **CFG-2:** `--configurator` mode: engine boot, embedded UI shell, bridge, Playback + System
  tabs, reset-all, save/quit, L1 smoke.
- **CFG-3:** Controls tab: remap picker, capture mode, validator, input tester.
- **CFG-4:** Packaging (desktop file, wrapper, icon, metainfo, flatpak-lint), Start-hold chord,
  SUPPORT.md, L2 on-Deck pass.

Each phase lands with its tests; "implemented" ≠ "verified" — say which.

## 13. Open questions (decide at implementation, none block CFG-1)

1. Do `MediaPlay`/`MediaPause` work on Leanback? (10-min on-Deck spike decides two picker rows.)
2. Second icon: gear-badged main icon or distinct glyph? (Asset question only.)
3. Should "Save & launch Deckback" be the default affordance on the save menu? (Probably yes —
   the reason you're in Settings is to go watch something.)
