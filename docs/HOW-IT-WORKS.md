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
unit the project owns — except one self-update check on an LCD (Van Gogh). The LCD shares the code
path but has otherwise run nothing.

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

The engine is **Chromium-based Cobalt ("Chrobalt", `youtube/cobalt` trunk, Chromium M138)**,
built as a stock `content_shell`. The key architectural bet is that **almost nothing is patched
into the engine**. Instead, a separate supervising process — the *launcher* — drives the engine
entirely over the **Chrome DevTools Protocol (CDP)**. The `patches/` directory holds exactly
**two** small engine patches (Widevine registration, and switching off content_shell's web-test
harness to slim the build); everything else — controller input, identity, codec steering, touch
handling, overlays — is the launcher plus a JSON config file.

Why this shape:

- **Rebase-cheap.** Cobalt trunk moves; a thin patch surface (two small patches) survives yearly
  engine bumps far better than a fork with input/media/permission patches woven through Blink.
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
│      └─ XCB ───────────► gamescope touch mode (hover/passthrough) │
└──────────────────────────────────────────────────────────────────┘
        ▲                                    ▲
        │ config/app.json                    │ patches/ (two patches:
        │ (UA, URL, keymap, flags…)          │  Widevine CDM registration,
                                             │  web-test harness off)
```

1. **Cobalt engine** — a `gclient`-managed Chromium/Cobalt checkout, pinned by `DEPS.pin`,
   modified only by the rebaseable quilt series in `patches/`. In-tree toolchain:
   **C++17**, Chromium's hermetic Clang + in-tree libc++.
2. **`launcher/`** — a standalone C++ shim with its own CMake build, no Chromium checkout needed.
   Out-of-tree toolchain: **C++23**, must build clean on Clang ≥18 **and** GCC 14, `-Werror`.
   Minimal deps: libsystemd, libcurl, libpulse, libxcb. Everything below in §4 lives here.
3. **`config/`** — `app.json` (UA/URL/quality/keymap, hot-swappable) plus injected page scripts in
   `config/scripts/` (`av1_steering.js`, `no_pointer.js`, `touch_gestures.js`, `osd.js`, …) and
   the `steam_input.vdf` controller template.

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

Two facts about the stock `content_shell` shape the design:

- It **ignores `--user-agent`** (hardcodes a Chrome UA) and takes the start URL positionally. So the
  launcher boots the engine on **`about:blank`** with a remote-debugging port, then sets the TV UA
  and navigates *over CDP* before the first real load.
- Its real data-dir switch is **`--data-path`**, not `--user-data-dir` (which it silently ignores).

**`launcher/src/devtools.cpp`** is the keystone: a **dependency-free CDP client** — raw sockets
plus a hand-rolled RFC 6455 WebSocket — that discovers the page target and runs `Runtime.evaluate`,
`Page.navigate`, `Input.dispatch*`, `Network.setUserAgentOverride`, and friends. It re-arms sticky
state (UA override, injected scripts) across Leanback's periodic target teardown, and holds every
other caller back until that re-arm finishes, so no navigation reaches a fresh target without the TV
UA. Every handle (the Navigator, the PlayerController, the input layer, …) shares one WebSocket. It
never sends `Network.enable`: the UA override does not need it, and it would stream an event for
every media segment.

---

## 4. What's built, subsystem by subsystem

### Engine bring-up ✅ Verified
The M138 `content_shell` (stock plus the two patches) boots and renders interactive Leanback under
gamescope's Xwayland, on the real RADV GPU (no SwiftShader), using the **Chromium media pipeline**
(not Starboard/`SbPlayer`). *Verified on M138, 2026-07-10 (the hardware-decode run).*

### Controller input + keymap ✅ Verified (buttons) / 🟡 Implemented (the rest)
**`launcher/src/input.cpp`** reads raw evdev directly from `<linux/input.h>` (no libevdev), opens
every gamepad-capable node and merges them, and translates buttons/axes into DOM key events
dispatched over CDP. Under Steam Input the readable device is the virtual Xbox-360 pad. The mapping
comes from `app.json:keymap`: each value is either a **verbatim DOM key** (`Enter`,
`MediaPlayPause`, `c`) or a **semantic alias** (`select`, `back`, `playpause`) resolved to a key —
the verbatim form wins, which is the hot-swap lever. A DOM key resolving to nothing (see below) is
logged as an unmapped WARN at startup rather than guessed.

Default bindings: A→Enter (select), B→Escape (back), X→MediaPlayPause, LB/RB→ArrowLeft/Right
(scrub), LT/RT→previous/next chapter, View→captions, Menu→Settings, Y→touch-gesture toggle, D-pad +
left stick→arrows. Chapter seek, captions and the gesture toggle are launcher actions, not
keystrokes: chapter seek evaluates `config/scripts/chapter_seek.js` over CDP (chapters come from the
TVHTML5 `/next` endpoint; a video without chapters falls back to a fixed ±`skip_seconds` jump),
captions toggle the player's caption module (Leanback ignores the desktop `c` hotkey), and Y acts
only when `touch_gestures` is on.

- ✅ Buttons + multi-device merge (*2026-07-08*), 2 s hotplug rescan (*2026-07-09*), and chapter
  seek (*2026-07-13*).
- 🟡 Auto-repeat **acceleration**, right-stick fast-scroll (`input.cpp` scales the arrow-repeat rate
  by stick deflection), and the context/modifier keymap **layers**
  (`layers.cpp`: `keymap_player`/`keymap_osk` overrides, `keymap_lt`/`keymap_rt` held-trigger
  layers — all four ship **empty**, mechanism only). None has run on a device.
- ⬜ `player_menu` and `scan_rewind`/`scan_forward` resolve to no key — Leanback publishes no
  verified key for them and Deckback refuses to guess. Text entry / on-screen-keyboard end-to-end
  is **not built**.

### Identity — the TV user-agent ✅ Verified
`navigator.cpp` arms a **sticky** `Network.setUserAgentOverride` with the Cobalt TV UA
(`Mozilla/5.0 (ChromiumStylePlatform) Cobalt/26.lts.0,gzip(gfe)`) *before* the first navigation and
re-arms it across target teardown. A cast-receiver UA drops into an unusable "Ready to cast"
screen; a desktop/SteamDeck UA selects a bundle with opaque overlay backgrounds. This is risk R1 —
the biggest external dependency — hence the hot-swappable config and the nightly `just smoke`
canary that re-asserts it against production. *Verified 2026-07-08.*

### AV1 codec steering 🟡 Implemented
`navigator.cpp` injects `config/scripts/av1_steering.js` at document-start (CDP
`Page.addScriptToEvaluateOnNewDocument`), overriding `MediaSource.isTypeSupported`,
`canPlayType`, and `mediaCapabilities.decodingInfo` so AV1 reports unsupported and YouTube serves
VP9/H.264. Patch-free; gated by `quality.steer_av1_unsupported` (default on). The JS logic is
verified offline; that YouTube *then* serves VP9 on-Deck is not yet proven. Steering stays on
permanently because the AV1-hardware-decode question can never be closed for the untested LCD.

### Touch handling ✅ Verified (disable_touch, gestures)
Under gamescope a stray finger arrives as synthetic mouse events that can navigate Leanback by
accident. `disable_touch` (default **on**) makes the panel inert two independent ways:

- **Page-side swallow** — `navigator.cpp` injects `config/scripts/no_pointer.js`, which captures and
  cancels every pointer/mouse/touch event in the page and hides the cursor (`cursor:none`).
- **Compositor hover-lock** — `touchmode.cpp` uses XCB to hold gamescope's
  `STEAM_TOUCH_CLICK_MODE` property at *hover* (0 — cursor moves, no click) while our window is
  focused, leaving the Steam overlay's own touch alone.

*Verified 2026-07-10:* at hover, a tap produced 45 mouse-moves and **0 clicks** and did not
navigate; the cursor was gone.

**Touch gestures (opt-in).** `touch_gestures` (default **off**) is the opposite of
`disable_touch`; if both are on, `disable_touch` wins and the log says so. `touchmode.cpp` holds
gamescope at touch mode **4 (passthrough)**, the only mode that delivers real `wl_touch` to the page.
`config/scripts/touch_gestures.js` recognises gestures in a capture-phase listener and queues them;
`gestures.cpp` polls the queue over CDP and turns each into a trusted key event or a player call.
Swipe moves the selection, tap selects, double-tap on the left/right third seeks (and accumulates),
holding the left third plays at 0.5× and the right at 2×, and a left-edge swipe goes Back; an
indicator shows each seek and hold, and Y (= the R4 grip) toggles the layer with a toast and a
rumble. Multitouch is delivered, but a touch with a second finger is ignored on purpose.
`navigator.maxTouchPoints` reads 0 although touch works, so support cannot be feature-detected —
hence the config gate. *Verified by finger 2026-07-31 (OLED);* the rumble and the seek indicator
past six double-taps are not. See
[`.internal/findings/durable/touch-gestures.md`](../.internal/findings/durable/touch-gestures.md).

An `EVIOCGRAB` lock of the touch panel does not work on SteamOS: the launcher cannot open the panel
node as the seat user, and **gamescope, not the app, reads the panel**. There is no such code; old
`touch_lock_*` config keys parse and are ignored. See
[`.internal/findings/durable/touch-lock.md`](../.internal/findings/durable/touch-lock.md).

### Hardware video decode (VA-API) ✅ Verified (OLED)
`config/app.json` **ships hardware decode**: `--enable-features=VaapiVideoDecodeLinuxGL` and
`--use-angle=gl` select `VaapiVideoDecoder`. It was verified on-Deck through the Flatpak in Game
Mode during real Leanback VP9 playback, under a dual gate: `VaapiVideoDecoder` +
`kIsPlatformVideoDecoder`, **and** a composited screenshot through the pixel-corruption check (0.0%
green-band rows across 5 frames, visually confirmed). `just power` measured **5.6 W** under
hardware-decoded VP9, inside the ≤ ~9 W gate. The LCD unit is untested. See
[`.internal/findings/milestones/m138.md`](../.internal/findings/milestones/m138.md) §"Media &
VA-API".

**Re-run the pixel check after every Cobalt bump.** Display-layer corruption is invisible to the
media pipeline: the decoder name and `corruptedVideoFrames=0` can read healthy while every frame is
striped green (a broken ANGLE import of the tiled NV12 VA surface does exactly that). See
[`.internal/findings/durable/hardware.md`](../.internal/findings/durable/hardware.md).

### Sleep / resume + idle-inhibit ✅ Verified
`platform.cpp` talks to **logind over sd-bus**, and reconnects with backoff if the system bus drops
(`busloop.cpp`, shared with the updater). On `PrepareForSleep` it holds a delay inhibitor while
`player.cpp` pauses the `<video>` and logs its position. On resume the player's own thread, not the
bus thread, waits for the network via `netprobe.cpp` (non-blocking TCP-connect with exponential
backoff to `youtube.com:443`), then nudges playback, reloading only if the sleep exceeded
`resume_reload_after_ms` (0 = never, the default). A suspend that arrives during that wait cancels
the nudge. While a video plays it holds a logind **block inhibitor** so the screen doesn't dim,
driven by a real play-state poll. *Verified 2026-07-10:* `just soak` ran 10 `rtcwake` suspend/resume
cycles and on **every** resume the app was alive with `currentTime` advanced, audio restored, and
the screen undimmed. `rtcwake` bypasses logind, so soak never sends `PrepareForSleep`; the
launcher's own hooks were verified by two power-button sleeps (*2026-10-21*), each logging
`PrepareForSleep(false)` → network reachable → resume nudge sent, with the video advancing after.
Before sleep the video is paused and its position logged. The engine closes its DevTools socket as
the sleep starts, so the pause retries once on a fresh session (verified on two power-button sleeps,
*2026-10-21*).

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
`app.json`** so it never advertises a control that does nothing; shown once at first run. `overlay.cpp`
injects a DOM toast; `haptic.cpp` uploads an `FF_RUMBLE` effect via `EVIOCSFF`. The card/toast were
CDP-injected over real Leanback and needed a **Trusted Types policy** to render (bare `innerHTML` is
a silent no-op on youtube.com/tv) — that part is verified, and the same toast confirms the
touch-gesture toggle on-Deck. Unit-tested; the card's survival across Leanback DOM swaps and the
rumble path have not been verified on a Deck.

### Settings menu (OSD) + self-update ✅ L2-verified (OLED) / 🟡 parts
`osdmenu.cpp` + `config/scripts/osd.js` are an in-app, controller-driven **Settings** menu, opened
from a persistent top-right button (Menu ☰) — the app's engineering surface. Tabs: **Settings**
(sub-tabs ‹ Keys | Captions ›), **Updates**, **About**. **Settings ▸ Keys** shows the live keymap
(reusing the controls-card derivation so the two can't disagree); **Settings ▸ Captions** is the
first *writable* surface — an ordered preferred-language list (add/remove + a full-language picker,
L1/R1 page-jump), the author/auto source policy, remember-last, and the toast. Edits persist to a
sparse `user.json` overlay (`config_store.cpp`, applied over `app.json`) and apply live to the View
caption toggle with no restart. The focus/tab/scroll state lives in the injected JS; `input.cpp`
captures the pad
modally and forwards discrete commands (`up/down/select/back/tab_next/scroll_*`), reusing the
existing auto-repeat + right-stick machinery. It is CSP-safe (`adoptedStyleSheets` + CSSOM, no
`<style>`, no `innerHTML`) and keep-alive'd against Leanback body swaps. Its invariant is *capture ⇔
paint*: a wiped overlay either re-injects or drops modal capture, so keys are never swallowed behind
a menu that isn't on screen. Menu opens it over playback
too; only the on-screen button hides there. Scroll regions move by CSS transform, because Leanback
resets `scrollTop` on any element we inject. **Exit:** holding **Y** for 600 ms anywhere in the menu
(the *Hold to exit* chip in the hint bar fills; an early release cancels, and a tap keeps meaning
*Ignore this update*) rumbles (rumble unverified on hardware), pauses the video, and shuts down through the watchdog's clean path —
exit 0, no restart — back to the Steam library. **Self-update lives here:**
`updater.cpp` (Flatpak-portal detect/deploy, verified on-Deck) does the work, and `updateprompt.cpp`
feeds the Updates tab + the button's amber badge. Detection does not wait on the portal's shared
0–30 min timer: at launch and on **Check for updates** the
launcher reads the published repo's ref (65 bytes) and compares it with the running commit from
`/.flatpak-info` — a check that installs nothing; the portal stays the only installer. Deploy
progress and a **Close now** button (the new commit binds on relaunch) are on the tab. L0-tested
(controller helpers, verdict parser, CSP-safe scripts); the L2 suite `tests/deck/test_osd.py`
(capture ⇔ paint, driven over CDP and a uinput pad) passes **8/8** on the OLED Deck (*2026-10-21*).
Not yet verified on a Deck: the ref check from inside the sandbox, deploy progress, and Close now.

### Error page + netprobe 🟡 Implemented
`errorpage.cpp` replaces Chromium's controller-unfocusable error screen with a kiosk **Try again**
page (focused button, Enter/Space; Escape deliberately does nothing so a network blip can't quit
the app), retrying with exponential backoff. `error_title`/`error_hint` are an R1 hotfix surface.
Unit-tested for detection + HTML escaping; on-Deck rendering untested.

### Widevine / DRM 🟡 Implemented — no real CDM ever loaded
The Widevine engine patch (`patches/0001-…-widevine-cdm-registration`) adds the shell-side
`AddContentDecryptionModules` override that stock `content_shell` lacks, registering a concrete
`CdmCapability`; `enable_widevine=true` in `args/deck.gn`. `cdm_fetcher.cpp` detects an installed
CDM and, only if the user opts in via `cdm_url`+`cdm_sha256`, downloads it, **verifies the SHA-256**
(self-contained `sha256.cpp`), and installs it. The project never bundles or redistributes Google's
CDM. *Built into a real engine and run 2026-07-10:* with no CDM installed it correctly registers
nothing — ClearKey resolves, `com.widevine.alpha` rejects with `NotSupportedError`. No real CDM has
ever been loaded and no DRM frame has ever played. DRM is resolution-capped by design (L3 software).

### Voice search ⬜ Not built (withdrawn)
Voice can't be a keypress on this engine: Cobalt routes voice through a Starboard platform service
this Chromium-based build lacks. The launcher has no voice code until it can be reimplemented
against a real device; Y is the touch-gesture toggle. The mic auto-grant
(`Browser.grantPermissions`, `navigator.cpp`) remains and is unverified on hardware.

### Config system ✅ Verified (parsing)
`config.cpp` loads `config/app.json` (`schema_version: 1`) once at startup into a `Config` struct
of compile-time defaults, through a dependency-free RFC 8259 parser (`json.cpp`). Fields bind to
exact dotted paths, `$`-prefixed keys are comments, unknown keys are warned about, and the keymap
resolves DOM-key-over-alias. A unit test loads
the **real shipped** `app.json` and asserts config can't drift from the C++. The config is designed
to be remotely overridable/signable (P10) so Leanback breakage is hotfixable without a rebuild.

> Settings a user changes in-app go through the OSD menu (above) into the sparse `user.json`
> overlay, which only allowlisted keys can enter; `app.json` stays the shipped, hotfixable layer.

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
- **L2** — on-Deck automation over SSH+CDP (`tests/deck/`, `just test-deck`). Runs against the
  OLED Deck (`test_osd.py` passes 8/8), but much of the suite is still blocked by Leanback's "watch
  as guest" account gate; those rows need a gate-dismiss fixture before they count as verified.
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
| **P2** | Session & lifecycle | 🟡 Partial (deep link ✅; error page + single-instance lock 🟡; 50-cycle gate ⬜) |
| **P3** | Input / controls | 🟡 Partial (buttons + hotplug + touch gestures ✅; OSK/latency/layers ⬜) |
| **P4** | Media pipeline & HW decode | ✅ HW decode ships on M138 (OLED, pixel-clean, 5.6 W); LCD untested; AV1 steering 🟡 |
| **P5** | Audio & microphone | 🟡 Partial (audio out + repair ✅; voice ⬜ withdrawn) |
| **P6** | Sleep / resume / idle-inhibit | ✅ Verified (soak incl. audio-restore + no-dim; launcher hooks via logind sleeps) |
| **P7** | Widevine path | 🟡 Registration patch built + run; no CDM ever loaded |
| **P8** | Packaging & distribution | ✅ Built + installed + self-updating, Steam artwork via the installer; 🟡 repo history/deltas/recall (rehearsed off-device, first production publish pending); ⬜ extra-data CDM wiring, <5-min gate |
| **P9** | Test & hardening (OLED matrix) | ⬜ Not started |
| **P10** | Flathub submission & maintenance | ⬜ Not started |
| **P11** | Focus/hover preview | ⬜ Not started (probe-first) |

## 7. The honest gaps

Permanent/structural (not backlog): the project owns **no LCD unit**, so apart from one self-update
check every result is OLED-only; power,
refresh-rate, and resume-timing numbers don't transfer to the LCD; the AV1 dispute can't be closed
for the LCD; auto-invoked on-screen keyboard on text focus and server-side controller glyphs are
impossible under Xwayland/without Steamworks; passing `js_mse_eme` is **not** certification.

Still unverified on hardware: auto-repeat acceleration, right-stick scroll, keymap layers, captions'
actual effect, mic capture, single-instance lock, rumble, audio device hot-swap, and hardware decode
on the LCD unit. Not built: voice search
(withdrawn), text-entry/OSK, input-latency measurement, Crashpad, forced 720p surface + glyph-size
check.
