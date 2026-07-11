---
scope: durable
verified: 2026-07-08
sources:
  - .internal/steamdeck-cobalt-youtube-plan.md
---

# Architecture (durable)

The layer model and the bets behind it. Names/flags change per milestone; the *shape* does not.

## Three layers, three lifecycles

1. **Cobalt engine** — gclient checkout pinned by `DEPS.pin`, modified via a rebaseable quilt
   series in `patches/` (never edited in place). Toolchain: hermetic, in-tree (C++17).
2. **`launcher/`** — standalone C++23 shim (own CMake, no Chromium checkout needed): env/flags,
   config, logind sleep-watcher, idle-inhibit, watchdog, CDM fetcher. Rebuilds in seconds.
3. **`config/`** — `app.json` (UA, URL, caps, keymap) + `steam_input.vdf`. Designed to be
   **remotely overridable and signed** so server-side Leanback breakage is hotfixable without a
   rebuild.

## Key architectural bets (doc §3)

- **Identity is UA-driven.** A console-class UA makes youtube.com/tv serve the full TV app. This is
  the single biggest external dependency and risk (R1). Keep it hot-swappable in `config/`.
- **Input at the browser/embedder layer, not JS injection.** Translate evdev/gamepad state into
  `ui::KeyEvent`s in C++ (lower latency, works before page JS, survives navigation) — unless a
  milestone spike shows Blink's Gamepad API already drives Leanback under the console UA.
- **App depends on Google's tolerance** of a TV-class UA (same posture as VacuumTube / Kodi
  plugins). Stated openly in the README.

## Launcher ↔ engine bridge: DevTools/CDP over the remote-debugging port

The launcher controls the running engine through the **Chrome DevTools Protocol**, not through any
Cobalt-specific API — so this survives Cobalt bumps. `launcher/src/devtools.{hpp,cpp}` is a
dependency-free CDP client: raw POSIX sockets + a hand-rolled RFC 6455 WebSocket client (we do *not*
use libcurl's experimental WS API). It discovers the page target via `GET /json/list`, upgrades to a
WebSocket, and issues `Runtime.evaluate` calls. Trust model: loopback only, so it skips
`Sec-WebSocket-Accept` verification (no crypto dependency).

`Runtime.evaluate` on `document.querySelector('video')` is the seam for everything the embedder needs
from page state — play/pause, `currentTime` checkpoint, is-playing. `PlayerController`
(`launcher/src/player.cpp`) uses it for the Phase 6 power contract (idle-inhibit while playing;
pause+checkpoint on suspend; nudge on resume). Requires `remote_debugging_port` set in `app.json`.
Cadence and the synthetic-activity fallback are config-driven (`devtools_poll_ms`,
`idle_inhibit_synthetic_fallback`). If a future spike needs richer control, prefer more CDP domains
over engine patches — keeps the launcher milestone-independent.

## Why the layers have different lifecycles

The engine is huge and churns yearly → isolate our changes as a thin patch series. The launcher is
tiny and stable → keep it out of the GN tree so it never needs a Chromium checkout. Config is the
fastest-changing surface (Leanback shifts under us) → make it data, not code, and updatable at
runtime.
