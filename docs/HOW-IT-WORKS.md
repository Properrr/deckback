# Deckback — How it works

This document describes **what Deckback actually is, what has been built, and how each piece
works**. It is deliberately honest about status: a feature being *implemented* is not the same as
being *verified on hardware*, and this page always says which one it means.

- The phased plan, locked decisions, and risk register live in
  [`.internal/steamdeck-cobalt-youtube-plan.md`](../.internal/steamdeck-cobalt-youtube-plan.md).
- The execution checklist is [`.internal/TASKS.md`](../.internal/TASKS.md); the honest
  tested-vs-untested matrix is [`.internal/TEST-PLAN.md`](../.internal/TEST-PLAN.md) §2.
- Hardware truths and locked findings live in [`.internal/findings/`](../.internal/findings/).

Every hardware result quoted here is from a Steam Deck **OLED (Sephiroth)** — the only physical
unit the project owns. No LCD (Van Gogh) unit exists; the LCD shares the code path but has run
nothing.

## Status legend

| Badge | Meaning |
|---|---|
| ✅ **Verified** | Exercised on the real Deck, with a dated result. |
| 🟡 **Implemented** | Code exists and passes unit/headless tests, but has **never been run on a Deck**. |
| ⛔ **Blocked** | Built and tried, but doesn't work on current hardware/software; reverted or disabled. |
| ⬜ **Not built** | Designed only, or deliberately deferred. |

---

## 1. The core idea

Deckback is an **unofficial native client for YouTube's TV interface (Leanback)** on the Steam
Deck, Game Mode only. It loads `https://www.youtube.com/tv` in a kiosk shell and adds controller
navigation, sleep/resume, audio recovery, and best-effort DRM.

The engine is **Chromium-based Cobalt ("Chrobalt", `youtube/cobalt` trunk, ~Chromium M114)**,
built as a stock `content_shell`. The key architectural bet is that **almost nothing is patched
into the engine**. Instead, a separate supervising process — the *launcher* — drives the unmodified
engine entirely over the **Chrome DevTools Protocol (CDP)**. The `patches/` directory holds exactly
**one** engine patch (Widevine registration); everything else — controller input, identity, codec
steering, touch handling, overlays — is the launcher plus a JSON config file.

Why this shape:

- **Rebase-cheap.** Cobalt trunk moves; a thin patch surface (one patch) survives yearly engine
  bumps far better than a fork with input/media/permission patches woven through Blink.
- **Hot-swappable.** Because identity (the TV user-agent) and the key map live in a JSON config
  read at startup — not compiled in — a server-side Leanback change can be worked around with a
  config push and a relaunch, no rebuild. This matters because depending on Google's TV interface
  is the project's single biggest external risk (R1).
- **Trusted input for free.** CDP `Input.dispatchKeyEvent` events arrive in the page as
  `isTrusted=true`, so the launcher can synthesize keystrokes the web app accepts as real — no
  in-engine input patch needed.

---

## 2. Architecture — three layers, three toolchains

```
┌──────────────────────────────────────────────────────────────────┐
│  launcher/  (C++23, standalone, no Chromium checkout)              │
│  supervises the engine and drives it over CDP                     │
│                                                                    │
│   main.cpp ── forks & watchdogs ──►  content_shell (the engine)   │
│      │                                    ▲                        │
│      │  devtools.cpp (CDP over raw        │  about:blank →         │
│      │  WebSocket) ───────────────────────┘  youtube.com/tv        │
│      │                                                             │
│      ├─ evdev gamepad ─► CDP key events                            │
│      ├─ logind/sd-bus ─► suspend/resume + idle-inhibit             │
│      ├─ libpulse ──────► audio un-mute repair                     │
│      └─ XCB ───────────► gamescope touch-mode hover-lock          │
└──────────────────────────────────────────────────────────────────┘
        ▲                                    ▲
        │ config/app.json                    │ patches/ (ONE patch:
        │ (UA, URL, keymap, flags…)          │  Widevine CDM registration)
```

1. **Cobalt engine** — a `gclient`-managed Chromium/Cobalt checkout, pinned by `DEPS.pin`
   (`a9181df8…`), modified only by the rebaseable quilt series in `patches/`. In-tree toolchain:
   **C++17**, Chromium's hermetic Clang + in-tree libc++.
2. **`launcher/`** — a standalone C++ shim with its own CMake build, no Chromium checkout needed.
   Out-of-tree toolchain: **C++23**, must build clean on Clang ≥18 **and** GCC 14, `-Werror`.
   Minimal deps: libsystemd, libpulse, libcurl. Everything below in §4 lives here.
3. **`config/`** — `app.json` (UA/URL/quality/keymap, hot-swappable) plus injected JS
   (`av1_steering.js`, `no_pointer.js`) and the `steam_input.vdf` controller template.

The two toolchains **must not mix** — see [CLAUDE.md](../CLAUDE.md) and
[`.internal/findings/durable/decisions.md`](../.internal/findings/durable/decisions.md).

---

## 3. The launcher supervisor

**`launcher/src/main.cpp`** is the orchestrator. On startup it:

1. Resolves directories (profile, state, logs) with XDG fallbacks — the Chromium profile lives in
   the Flatpak's durable data dir so sign-in survives updates (§4, *Persistent profile*).
2. Takes a **single-instance lock** (`flock` on `$XDG_RUNTIME_DIR/deckback.lock`).
3. Loads `config/app.json`.
4. Forks the engine via the **watchdog** and connects the **CDP client**.
5. Wires up every controller (input, player, platform, audio…) in dependency order, and tears them
   down in reverse on `SIGTERM`.

Two facts about the stock `content_shell` shaped the design, both discovered on-Deck:

- It **ignores `--user-agent`** (hardcodes a Chrome UA) and takes the start URL positionally. So the
  launcher boots the engine on **`about:blank`** with a remote-debugging port, then sets the TV UA
  and navigates *over CDP* before the first real load.
- Its real data-dir switch is **`--data-path`**, not `--user-data-dir` (which it silently ignores).

**`launcher/src/devtools.cpp`** is the keystone: a **dependency-free CDP client** — raw sockets
plus a hand-rolled RFC 6455 WebSocket — that discovers the page target and runs `Runtime.evaluate`,
`Page.navigate`, `Input.dispatch*`, `Network.setUserAgentOverride`, and friends. It re-arms sticky
state (UA override, injected scripts) across Leanback's periodic target teardown, and supports
several concurrent CDP clients (the Navigator and the PlayerController each hold their own).

---

## 4. What's built, subsystem by subsystem

### Engine bring-up ✅ Verified
A stock Chromium-M114 `content_shell` boots and renders interactive Leanback under gamescope's
Xwayland, on the real RADV GPU (no SwiftShader), using the **Chromium media pipeline** (not
Starboard/`SbPlayer`). *Verified 2026-07-08.*

### Controller input + keymap ✅ Verified (buttons) / 🟡 Implemented (the rest)
**`launcher/src/input.cpp`** reads raw evdev directly from `<linux/input.h>` (no libevdev), opens
every gamepad-capable node and merges them, and translates buttons/axes into DOM key events
dispatched over CDP. Under Steam Input the readable device is the virtual Xbox-360 pad. The mapping
comes from `app.json:keymap`: each value is either a **verbatim DOM key** (`Enter`,
`MediaPlayPause`, `c`) or a **semantic alias** (`select`, `back`, `playpause`) resolved to a key —
the verbatim form wins, which is the hot-swap lever. A DOM key resolving to nothing (see below) is
logged as an unmapped WARN at startup rather than guessed.

Default bindings: A→Enter (select), B→Escape (back), X→MediaPlayPause, LB/RB→ArrowLeft/Right
(scrub), View→`c` (captions), Menu→show controls card, D-pad + left stick→arrows.

- ✅ Buttons + multi-device merge (*2026-07-08*) and 2 s hotplug rescan (*2026-07-09*).
- 🟡 Auto-repeat **acceleration**, right-stick fast-scroll (`input.cpp` scales the arrow-repeat rate
  by stick deflection), and the context/modifier keymap **layers**
  (`layers.cpp`: `keymap_player`/`keymap_osk` overrides, `keymap_lt`/`keymap_rt` held-trigger
  layers — all four ship **empty**, mechanism only). None has run on a device.
- ⬜ `voice_search` (Y), `player_menu` (Start), `scan_rewind`/`scan_forward` (L2/R2) are
  **deliberately unbound** — Leanback publishes no verified key for them and Deckback refuses to
  guess. Text entry / on-screen-keyboard end-to-end is **not built**.

### Identity — the TV user-agent ✅ Verified
`navigator.cpp` arms a **sticky** `Network.setUserAgentOverride` with the Cobalt TV UA
(`Mozilla/5.0 (ChromiumStylePlatform) Cobalt/26.lts.0,gzip(gfe)`) *before* the first navigation and
re-arms it across target teardown. A cast-receiver UA drops into an unusable "Ready to cast"
screen; a desktop/SteamDeck UA selects a bundle with opaque overlay backgrounds. This is risk R1 —
the biggest external dependency — hence the hot-swappable config and the nightly `just smoke`
canary that re-asserts it against production. *Verified 2026-07-08.*

### AV1 codec steering 🟡 Implemented
`navigator.cpp` injects `config/av1_steering.js` at document-start (CDP
`Page.addScriptToEvaluateOnNewDocument`), overriding `MediaSource.isTypeSupported`,
`canPlayType`, and `mediaCapabilities.decodingInfo` so AV1 reports unsupported and YouTube serves
VP9/H.264. Patch-free; gated by `quality.steer_av1_unsupported` (default on). The JS logic is
verified offline; that YouTube *then* serves VP9 on-Deck is not yet proven. Steering stays on
permanently because the AV1-hardware-decode question can never be closed for the untested LCD.

### Touch handling ✅ Verified (disable_touch) / ⛔ Blocked (the old lock)
Under gamescope a stray finger arrives as synthetic mouse events that can navigate Leanback by
accident. `disable_touch` (default **on**) makes the panel inert two independent ways:

- **Page-side swallow** — `navigator.cpp` injects `config/no_pointer.js`, which captures and
  cancels every pointer/mouse/touch event in the page and hides the cursor (`cursor:none`).
- **Compositor hover-lock** — `touchmode.cpp` uses XCB to hold gamescope's
  `STEAM_TOUCH_CLICK_MODE` property at *hover* (0 — cursor moves, no click) while our window is
  focused, leaving the Steam overlay's own touch alone.

*Verified 2026-07-10:* at hover, a tap produced 45 mouse-moves and **0 clicks** and did not
navigate; the cursor was gone.

⛔ The earlier design — an `EVIOCGRAB` exclusive grab of the FTS3528 touch panel (`touch.cpp`,
toggled by an L3+R3 chord with toast + haptic) — is **proven dead** on SteamOS (physical test
*2026-07-10*): the launcher can't open the panel node as the seat user, and even with access the
grab doesn't block touch because **gamescope, not the app, reads the panel**. That machinery ships
disabled (`touch_lock_enabled: false`) and is retained only as a documented dead end. See
[`.internal/findings/durable/touch-lock.md`](../.internal/findings/durable/touch-lock.md).

### Hardware video decode (VA-API) ⛔ Blocked — ships software decode
Intended as the P4 battery win. Selecting `VaapiVideoDecoder` needs
`--enable-features=VaapiVideoDecodeLinuxGL` **and** `--use-angle=gl`. On-Deck those flags engaged
the hardware decoder **but painted green-band corruption on every frame** — ANGLE's DMA-buf import
of the tiled NV12 VA surface is broken on radeonsi. Worse, every automated metric false-passed
(`corruptedVideoFrames=0`, correct decoder name) while the panel was visibly striped. A 7-variant
bisection proved the corruption inseparable from `VaapiVideoDecoder`. The flags were **reverted
2026-07-10**; Deckback **ships software decode** (`VpxVideoDecoder`, clean, verified). Re-enabling
requires a Mesa/ANGLE-layer fix and is gated on a **pixel** check, never a decoder-name check
(`b3fcd58` added exactly that gate). See
[`.internal/findings/milestones/m114.md`](../.internal/findings/milestones/m114.md) §"VA-API decode
is VISUALLY CORRUPT".

### Sleep / resume + idle-inhibit ✅ Verified (core) / 🟡 (two clauses)
`platform.cpp` talks to **logind over sd-bus**. On `PrepareForSleep` it holds a delay inhibitor
while `player.cpp` pauses the `<video>` and logs its position; on resume it waits for the network
via `netprobe.cpp` (non-blocking TCP-connect with exponential backoff to `youtube.com:443`), then
nudges playback, reloading only if the sleep exceeded `resume_reload_after_ms` (0 = never, the
default). While a video plays it holds a logind **block inhibitor** so the screen doesn't dim,
driven by a real play-state poll. *Verified 2026-07-10:* `just soak` ran 10 `rtcwake` suspend/resume
cycles and on **every** resume the app was alive with `currentTime` advanced. 🟡 The "audio
restored" and "no screen dim" clauses are not yet separately gated.

### Audio recovery ✅ Verified
Chromium audio goes through PipeWire's Pulse socket. SteamOS sometimes leaves Deckback's stream
muted, and the sandbox is denied mute changes (`Access denied`). `audio.cpp` finds Deckback's own
sink-input via libpulse and unmutes it; because the in-sandbox call is denied, a **host-side user
service** (installed by `scripts/install-audio-repair.sh`) scoped to Deckback's sink-input does the
actual repair. *Verified 2026-07-09:* a live muted stream recovered to `Mute: no`.

### Watchdog / restart 🟡 Implemented
`watchdog.cpp` forks + `execv`s the engine, `waitpid`s it, and restarts on crash within a rate
limit (`max_restarts_per_minute`, default 5). `SIGINT`/`SIGTERM` forwards `SIGTERM` to the child
and exits 0, so Steam's "Close game" is clean. Unit-tested; the real Game-Mode close path and the
50-cycle stability gate have not run on a Deck.

### Persistent profile + deep link ✅ Verified
The Chromium profile resolves to the Flatpak's durable data dir
(`~/.var/app/io.github.properrr.deckback/data/…/profile`) and is passed as `--data-path`;
`profile.cpp` does staged atomic migration of a legacy profile without overwriting a durable one.
*Verified 2026-07-09:* sign-in survived a rebuilt-Flatpak reinstall + relaunch. `app://<video-id>`
deep links rewrite to a Leanback watch route (*verified 2026-07-08*).

### First-run controls card + toast/haptic 🟡 Implemented
`onboarding.cpp` injects a one-shot DOM card listing the control mapping, **derived from the live
`app.json`** so it never advertises a control that does nothing; re-openable via Menu. `overlay.cpp`
injects a DOM toast; `haptic.cpp` uploads an `FF_RUMBLE` effect via `EVIOCSFF`. The card/toast were
CDP-injected over real Leanback and needed a **Trusted Types policy** to render (bare `innerHTML` is
a silent no-op on youtube.com/tv) — that part is verified; the toast/haptic were built for the now-
dead touch lock and are effectively orphaned. Unit-tested; the DOM-survival and rumble paths have
not run on a Deck.

### Error page + netprobe 🟡 Implemented
`errorpage.cpp` replaces Chromium's controller-unfocusable error screen with a kiosk **Try again**
page (focused button, Enter/Space; Escape deliberately does nothing so a network blip can't quit
the app), retrying with exponential backoff. `error_title`/`error_hint` are an R1 hotfix surface.
Unit-tested for detection + HTML escaping; on-Deck rendering untested.

### Widevine / DRM 🟡 Implemented — no real CDM ever loaded
The **only** engine patch (`patches/0001-…-widevine-cdm-registration`) adds the shell-side
`AddContentDecryptionModules` override that stock `content_shell` lacks, registering a concrete
`CdmCapability`; `enable_widevine=true` in `args/deck.gn`. `cdm_fetcher.cpp` detects an installed
CDM and, only if the user opts in via `cdm_url`+`cdm_sha256`, downloads it, **verifies the SHA-256**
(self-contained `sha256.cpp`), and installs it. The project never bundles or redistributes Google's
CDM. *Built into a real engine and run 2026-07-10:* with no CDM installed it correctly registers
nothing — ClearKey resolves, `com.widevine.alpha` rejects with `NotSupportedError`. No real CDM has
ever been loaded and no DRM frame has ever played. DRM is resolution-capped by design (L3 software).

### Voice search 🟡 Implemented — ships disabled
Voice can't be a keypress on this engine (Cobalt routes voice through a Starboard service this
build lacks). `voice.cpp` instead does hold-to-talk by clicking Leanback's own on-screen mic button
over CDP (`voice_mic_selectors` candidate list), with audio ducking and a mic auto-grant
(`Browser.grantPermissions`). Ships **off** (`voice_enabled: false`); the gating question — whether
Leanback even renders that mic button under our UA — is unanswered.

### Config system ✅ Verified (parsing)
`config.cpp` loads `config/app.json` (`schema_version: 1`) once at startup into a `Config` struct
of compile-time defaults. It's a hand-rolled top-level-key extractor (not a full JSON parser),
skips `$`-prefixed comment keys, and resolves the keymap DOM-key-over-alias rule. A unit test loads
the **real shipped** `app.json` and asserts config can't drift from the C++. The config is designed
to be remotely overridable/signable (P10) so Leanback breakage is hotfixable without a rebuild.

> A planned **companion "Deckback Settings" app** would give this config a GUI (toggles, key
> remapping, reset-to-defaults) — design only, nothing built yet. See
> [`.internal/configurator-plan.md`](../.internal/configurator-plan.md).

### Packaging (Flatpak / zypak) ✅ Verified (built + installed)
`flatpak/io.github.properrr.deckback.yml` on `org.freedesktop.Platform//25.08`, sandboxed via
**zypak** (no `--no-sandbox`). `finish-args` grants `--device=input` (never `--device=all`),
network, x11+ipc, pulse, dri, ScreenSaver and login1, and requires flatpak ≥1.16 (which learned
`--device=input`). `scripts/flatpak-lint.sh` fails the build if the manifest ever drops evdev, and
`just install` reads the installed permissions back and refuses if evdev is absent. The `pack` stage
is the one part built on `debian:13-slim` rather than Debian 12. *Verified 2026-07-10:* built,
installed, and the sandbox read back `devices=dri;input;`; Game-Mode launch + `steamos-add-to-steam`
verified 2026-07-08.

---

## 5. Build & test surface

Builds run on a **workstation/CI, never on the Deck** (the Deck is a deploy/test target over SSH).
Everything happens inside a Debian-12 Docker container matching Cobalt trunk. The tree must sit on a
real Linux filesystem — a WSL `/mnt/*` mount is case-insensitive and drops mode bits, and Chromium
won't build correctly there. Measured footprint is ≈45 GB (source + one preset + caches).

`just` is the command runner; every recipe delegates to `scripts/*.sh` so CI can call scripts
directly. Key recipes: `bootstrap`, `sync` (gclient at `DEPS.pin` + re-apply patches), `gen`/`build`
(presets `dev`/`deck`/`asan`), `launcher` (build+test the shim with no Chromium tree), `smoke` (the
headless CI gate + R1/R2 canary), `deploy`/`run`/`remote-run`/`logs`/`debug`, `test-deck` (on-Deck
pytest over SSH+CDP), `soak`/`power`, `cert` (`js_mse_eme` conformance), `flatpak`/`install`/
`release`. See [`.internal/HARNESS.md`](../.internal/HARNESS.md) for the exit-code taxonomy
(2 = product wrong, 3 = environment, 4 = transport) and which machine each recipe runs on.

**Test tiers** (see [`.internal/TEST-PLAN.md`](../.internal/TEST-PLAN.md)):

- **L0** — launcher unit tests + pure-logic harness. Real, green.
- **L1** — headless Xvfb boot + CDP assertions + screenshot (`just smoke`). Real, green.
- **L2** — on-Deck automation over SSH+CDP (`tests/deck/`). Built; **ran against a Deck for the
  first time 2026-07-10**, but partly compromised by Leanback's "watch as guest" account gate — most
  rows still need a gate-dismiss fixture before they count as verified.
- **L3** — MSE/EME/codec conformance (`js_mse_eme`, pinned). Ran 2026-07-10: **45/46** (one
  documented harness failure, same as the workstation). Not YouTube certification — see
  [`legal.md`](legal.md).
- **L4** — human/camera/multimeter checks. Never run.

---

## 6. Phase status

| Phase | Scope | Status |
|---|---|---|
| **P0** | Research / de-risking spikes | ✅ Done |
| **P1** | Build & bring-up on Deck | 🟡 Partial (profile ✅; forced-720 surface, Crashpad, 30-min gate ⬜) |
| **P2** | Session & lifecycle | 🟡 Partial (deep link + error page 🟡; 50-cycle gate ⬜) |
| **P3** | Input / controls | 🟡 Partial (buttons + hotplug ✅; OSK/latency/layers/touch-gesture ⬜) |
| **P4** | Media pipeline & HW decode | ⛔ Blocked (VA-API corrupt, reverted; ships software; AV1 steering 🟡) |
| **P5** | Audio & microphone | 🟡 Partial (audio out + repair ✅; voice ⬜/disabled) |
| **P6** | Sleep / resume / idle-inhibit | ✅ Core verified 2026-07-10 (audio-restore + no-dim clauses 🟡) |
| **P7** | Widevine path | 🟡 Registration patch built + run; no CDM ever loaded |
| **P8** | Packaging & distribution | ✅ Built + installed; ⬜ extra-data CDM wiring, grid art, <5-min gate |
| **P9** | Test & hardening (OLED matrix) | ⬜ Not started |
| **P10** | Flathub submission & maintenance | ⬜ Not started |
| **P11** | Focus/hover preview | ⬜ Not started (probe-first) |

## 7. The honest gaps

Permanent/structural (not backlog): every result is OLED-only and **no LCD unit exists**; power,
refresh-rate, and resume-timing numbers don't transfer to the LCD; the AV1 dispute can't be closed
for the LCD; auto-invoked on-screen keyboard on text focus and server-side controller glyphs are
impossible under Xwayland/without Steamworks; passing `js_mse_eme` is **not** certification.

Still unverified on hardware: auto-repeat acceleration, right-stick scroll, keymap layers, captions'
actual effect, mic capture, **voice search end-to-end**, single-instance lock, resume audio-restore,
audio device hot-swap, and any hardware-decode path. Not built: text-entry/OSK, touch gesture layer,
input-latency measurement, Crashpad, forced 720p surface + glyph-size check, grid art.
