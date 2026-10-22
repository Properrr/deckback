# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Current state

"Implemented" is not "verified" — say which you mean. Phase status lives in `.internal/TASKS.md`;
the tested-vs-untested matrix is `.internal/TEST-PLAN.md` §2. Read both before repeating any
status claim.

- **Hardware coverage:** every hardware result is from the **OLED** unit. The LCD unit has run
  nothing except one self-update round-trip (`durable/self-update.md`).
- **Verified on hardware (OLED):** gamepad input + hotplug, TV UA, VP9 **hardware** decode (M138,
  pixel-clean) under AV1 steering, persistent profile, deep link, audio + host-side mute repair,
  Game Mode launch, chapter seek, L3 conformance (45/46, same as the workstation), the OSD menu's
  L2 suite (`tests/deck/test_osd.py`, 8/8), and CDP-injected overlays (controls card, toast)
  rendering over real Leanback. Overlays need a Trusted Types policy: a bare `innerHTML` is a
  silent no-op on youtube.com/tv (`milestones/m138.md`).
- **Hardware decode ships** on M138 (cobalt-27, the current pin):
  `--enable-features=VaapiVideoDecodeLinuxGL --use-angle=gl` are in `config/app.json`
  `cobalt_flags`. Verified on-Deck through the Flatpak under the DUAL gate —
  `VaapiVideoDecoder`+`kIsPlatformVideoDecoder` **and** a composited screenshot through
  `video_corruption_verdict` (0.0% green-band rows across 5 frames). The decoder name and
  `corruptedVideoFrames=0` can read healthy while every frame is corrupt, so **after any Cobalt
  bump, do not trust these flags until the PIXEL check passes again** (`durable/hardware.md`).
- **Measured (OLED):** P4 power — `just power` averaged **5.6 W** under HW-decode VP9 playback
  (gate ≤~9 W). P6 suspend/resume — `just soak` passed: app alive and video advancing after every
  `rtcwake` resume, audio restored, no screen dim. `rtcwake` **bypasses logind**, so soak never
  sends `PrepareForSleep` and never runs the launcher's own sleep/resume hooks; those are verified
  by power-button sleeps (TEST-PLAN §1). `just soak` needs passwordless `sudo rtcwake` (a
  `zz-`-prefixed sudoers drop-in; see the script's own error text).
- **Touch** (`durable/touch-lock.md`, `durable/touch-gestures.md`):
  - `disable_touch` (default **on**) makes the panel inert two ways: the Navigator injects
    `config/scripts/no_pointer.js` to swallow every pointer/mouse/touch event in the page, and
    `launcher/src/touchmode.cpp` holds gamescope's global `STEAM_TOUCH_CLICK_MODE` at hover (0)
    while our window is focused. Verified: at hover a tap gives mousemoves, 0 clicks, no navigation.
  - The `EVIOCGRAB` touch **lock is dead**, not untested: the seat user `deck` cannot `open()` the
    panel node (`--device=input` changes visibility, not ownership), and gamescope, not the app,
    reads the panel. Its code is deleted; old `touch_lock_*` keys parse and are ignored.
  - `touch_gestures` (default **off**, mutually exclusive with `disable_touch`): at gamescope touch
    mode **4 (passthrough)** real `wl_touch` reaches Blink; a capture-phase page script queues
    gestures and the launcher polls the queue and turns them into trusted keys or player calls.
    Swipe, tap, double-tap seek, hold for 0.5×/2×, left-edge-swipe Back, and a **Y (= the R4
    grip) toggle** — each driven by a finger on-Deck.
  - **Multitouch is delivered** (the router's `multiFinger` probe hit 5 in 48 sequences); how
    *many* points arrive is unknown and gates any pinch work.
  - **`navigator.maxTouchPoints` reads 0** on a panel where touch demonstrably works, so nothing
    can feature-detect touch: a gate on `maxTouchPoints` / `'ontouchstart'` / `pointer: coarse`
    disables touch exactly where it works. That is why gestures are config-gated.
- **Not verified on hardware:** auto-repeat acceleration, mic capture, single-instance lock, rumble.
  Voice search is withdrawn: removed from the launcher, it never worked on this engine.
- **L2** (`just test-deck`, on-Deck over SSH+CDP) runs against the OLED Deck, but much of the suite
  is still blocked by Leanback's guest-account gate (TEST-PLAN §1). Treat "L2" claims dated before
  2026-07-10 as never-executed.

The authoritative design doc is `.internal/steamdeck-cobalt-youtube-plan.md` —
read it in full before doing any implementation work. It defines the phased plan (P0–P10), locked
decisions, risk register, toolchain rules, container setup, and the complete `just` command surface.
This CLAUDE.md summarizes the non-obvious parts; the plan is the source of truth and wins on
conflict — **except where a registered finding contradicts it**, since findings record what the
hardware actually did. Check `.internal/TASKS.md` for real status before trusting any phase claim
here. **All agent/planning docs live under `.internal/`** (design doc,
`.internal/TASKS.md` execution checklist, `.internal/TEST-PLAN.md` test tiers + honest
tested/untested matrix, `.internal/HARNESS.md` the build/deploy/test command surface + exit-code
taxonomy, `.internal/findings/` registered findings, `.internal/MIGRATION.md`);
`docs/` holds public deliverables (`HOW-IT-WORKS.md` the built-state/how-it-works overview,
including the in-app OSD Settings menu,
`SUPPORT.md`, `legal.md`).

**Before running or adding any `just` recipe, read `.internal/HARNESS.md`.** It defines the exit-code
taxonomy (`2` = the product is wrong; `3` = environment; `4` = transport — retry these, never a `2`),
which machine each recipe runs on, and which recipes still need a human. Recipes that decide pass/fail
must have L0 coverage in `tests/harness/`: a check that cannot fail is not a check.

**Never push without `just preflight` green; never release from a commit that is not green in CI.**
`scripts/preflight.sh` (`just preflight`) is the ONE definition of the pre-push / pre-release gate —
shellcheck + the harness suite + clang-format + the launcher gcc/clang builds + gn-args. Both
`.githooks/pre-push` and CI (`.github/workflows/lint.yml`) call it, so local and CI cannot drift;
`just release` refuses to build unless the tagged commit's CI is green (`FORCE=1` overrides). Run
`just hooks` once per clone — it is mandatory, not optional. clang-format is pinned to **23.1.3**
(PyPI, `CLANG_FORMAT_VERSION` in `scripts/lib.sh`, used by preflight, `just fmt` and CI alike;
other clang-format releases format differently); the launcher build needs `libxcb1-dev`/`libpulse-dev` to match CI. See
`.internal/findings/durable/preflight-parity.md`.

**Before claiming anything works, check `.internal/TEST-PLAN.md` §2.** Unit tests here prove pure
functions; they prove nothing about Leanback, gamescope, or the GPU. Every hardware result so far
is from the **OLED** unit, and several shipped features have **zero** verification. New features
get a failing on-device test first (§0).

**Where knowledge lives.** Register findings in `.internal/findings/`, split into `durable/`
(survives Cobalt bumps: architecture, hardware/AV1 facts, strategy, locked decisions) and
`milestones/<mNNN>.md` (version-scoped: GN args, working UA, VA-API/CDM state, patch-rebase — keyed
to `DEPS.pin`, presumed stale on a bump). The yearly Cobalt-bump procedure is
`.internal/MIGRATION.md`. Before trusting a version-specific fact, confirm which layer it's in.

## What this is

An unofficial native YouTube TV (Leanback) client for Steam Deck (LCD/Van Gogh + OLED/Sephiroth),
SteamOS 3.x, **Game Mode only**. Engine: Chromium-based Cobalt ("Chrobalt", `youtube/cobalt`
trunk, Chromium M138). It loads `https://www.youtube.com/tv` in a kiosk shell and adds
controller input, touch handling, hardware decode, sleep/resume, an in-app settings menu,
self-update, and Widevine (best-effort L3). Mic/voice search is in the plan but currently withdrawn.

Branding constraint: never put "YouTube"/"YT" in the app id or name; app id is `io.github.<you>.<name>`.

## Architecture (the big picture)

Three cooperating layers, each with a **different toolchain** (see below):

1. **Cobalt engine** — a `gclient`-managed Chromium/Cobalt checkout, pinned by `DEPS.pin` and
   modified via a rebaseable **quilt patch series in `patches/`** (never edited in place). Gamepad
   input, `MediaCapabilities`/AV1 steering, and the mic auto-grant live in the launcher over CDP,
   not in patches. `patches/series` holds exactly **two** entries: (1) Widevine CDM registration,
   because the shell has no `AddContentDecryptionModules` override and `enable_widevine` alone
   loads nothing (the patch's own header explains why) — it compiles, links, and regresses neither
   `just smoke` nor `just cert`; with no CDM installed it correctly registers nothing, and
   `com.widevine.alpha` rejects with `NotSupportedError`; **no real CDM has ever been loaded** — CI
   must never fetch Google's (`docs/legal.md`); (2) `support_web_tests=false` in content/shell —
   content_shell is upstream's web-test shell and otherwise always links ~1,134 TUs of dead test
   harness (see `.internal/findings/durable/build-slimming.md`, which also documents the
   feature-off GN args in `args/common.gn`: −9.5% stripped binary, no SwiftShader, smoke-verified —
   on-Deck gates pending).
2. **`launcher/`** — a small standalone C++ shim (its own CMake build, no Chromium checkout
   needed): env/flags/config, evdev gamepad→CDP key injection, touch handling (gamescope touch
   mode + gesture router), logind sleep-watcher over D-Bus, idle-inhibit manager, startup CDP
   policy (TV UA, AV1 steering, mic grant), OSD settings menu + self-update, CDM fetcher,
   watchdog restart.
3. **`config/`** — `app.json` (UA string, URL, quality caps, keymap — designed to be
   **remotely overridable/hot-swappable** so Leanback server-side breakage can be hotfixed without
   a rebuild), injected page scripts in `config/scripts/`, and the `steam_input.vdf` controller
   template.

Key bets: identity is UA-driven (biggest external risk — R1); input is handled at the embedder/C++
layer, not JS injection; the app depends on Google's tolerance of the TV UA (state this openly).

## Two toolchains — do not mix

- **In-tree (`patches/`, Cobalt):** **C++17 only.** Use Chromium's hermetic bundled Clang, lld,
  and in-tree libc++ (`use_custom_libcxx=true`). Do NOT use system Clang/GCC or C++20+ — version
  skew breaks `-Werror`/plugins/LTO and leaking C++20 maximizes rebase pain. Format with the tree's
  `buildtools/linux64/clang-format`.
- **Out-of-tree (`launcher/`, tools):** **C++23**, must build cleanly on **both Clang ≥18 and
  GCC 14** (Flatpak SDK is GCC-based), `-Wall -Wextra -Werror`. Minimal deps only: libsystemd,
  libcurl, libpulse, libxcb (evdev is read raw via `<linux/input.h>`). No Boost/Folly-class
  dependencies.

## Build & dev workflow

Builds run on a **workstation/CI, never on the Deck**. Measured footprint for a `--no-history`
checkout + one preset: source tree 22 GB, `out/deck` 9.9 GB, sccache 787 MB, flatpak cache 3.1 GB,
docker images ~3 GB — **≈45 GB total**. Put the tree on a
real Linux filesystem: a WSL `/mnt/*` (9p) mount is case-insensitive, drops mode bits, and is ~100×
slower at file creation — Chromium will not build correctly there.
The Deck is a deploy/test target over SSH only. All work happens inside a Docker/Podman
container based on **Debian 12 (bookworm)** — matching Cobalt trunk, which builds every image on
`marketplace.gcr.io/google/debian12` (see `docker/Dockerfile`; base is a hermetic-toolchain "tool
caddy", so glibc is required — no Alpine/musl). The `pack` stage is the deliberate exception
(`PACK_BASE=debian:13-slim`); everything that touches Chromium is bookworm. Source is bind-mounted, never baked in. The image's
package lists are mirrored from `cobalt/docker/{linux,unittest}` and must be re-synced on each
Cobalt bump (`.internal/MIGRATION.md`). `sccache` lives in a host bind-mount (`SCCACHE_HOST_DIR`).

Command runner is **`just`** (every recipe delegates to `scripts/*.sh` so CI can call scripts
directly). Core recipes — see `.internal/HARNESS.md` §3 for the full table:

- `just bootstrap` — one-time: build image, `gclient config` + first sync at `DEPS.pin`.
- `just sync` — `gclient sync -r $(cat DEPS.pin) --no-history` (shallow, single revision — Cobalt's
  git history is untracked; `DEPS.pin` is the pin, not a submodule) then re-apply `patches/`. Refuses
  a dirty tree.
- `just gen <preset>` / `just build <preset>` — `gn gen` / `autoninja` for a preset (default `dev`).
  Presets live in `args/`: `dev.gn` (component build, DCHECKs, fast iteration), `deck.gn`
  (static/official, deployable), `asan.gn`.
- `just compdb` — generate `compile_commands.json` for clangd.
- `just smoke` — Xvfb headless boot + DevTools-protocol assertion + screenshot (the CI gate; also
  run nightly against production youtube.com/tv as an R1/R2 canary for Leanback UI shifts).
- `just deploy [host]` / `just run` / `just logs` / `just debug` — rsync stripped build to
  `~/cobalt-yt/` on the Deck, launch over SSH, tail logs, open a `chrome://inspect` tunnel
  (`ssh -L 9222:localhost:9222`, remote-debugging-port 9222). Deck host from `.env` (`DECK_HOST`).
- `just patch-new <name>` / `just patch-refresh` — git-based quilt flow (commit in `cobalt/`,
  `git format-patch` into `patches/`, regenerate series).
- `just power` / `just soak [n]` — P4 battery gate (≤~9 W 1080p VP9) and P6 suspend/resume loop.
- `just flatpak` / `just release <tag>` — flatpak-builder bundle; tagged release build (+ThinLTO).

Packaging is Flatpak on `org.freedesktop.Platform//25.08`, sandboxed via **zypak** (not
`--no-sandbox` — that's rejected by Flathub). Widevine CDM is user-fetched at first run, never
redistributed by us. The **`pack` image is the one stage that is NOT Debian 12** — it is
`debian:13-slim`, because flatpak-builder compiles every module inside the SDK sandbox, so its
host distro never reaches the artifact, and bookworm's flatpak (1.14) cannot express
`--device=input` while its flatpak-builder (1.2.3) cannot install a metainfo file. Run
`just flatpak-lint` (no engine needed) before touching `flatpak/`; see
`.internal/findings/durable/packaging-toolchain.md`.

## Hard constraints to keep in mind

- **Steer YT to VP9/H.264, not AV1.** The stated reason ("no hardware AV1 decode on either Deck APU")
  is **disputed**: libva on the OLED unit advertises `AV1Profile0` under `VAEntrypointVLD`. We have
  never observed AV1 decoding *through* `VaapiVideoDecoder`, and the LCD unit is untested, so the
  steering stays — but do not repeat the "no AV1 hardware" claim as fact. See
  `.internal/findings/durable/hardware.md`.
- `proprietary_codecs = true` + `ffmpeg_branding = "Chrome"` are required in GN args or YT fallback
  (H.264/AAC/HEVC) formats break.
- `DEPS.pin` is the single source of truth for the Cobalt commit; bumping it is a reviewed change
  that must pass `just smoke` on both presets.
- Target render is 1280×720 letterboxed to the 800p panel, capped at 60 fps. HDR/90 Hz/4K/docked
  and ad-blocking are explicit non-goals for v1.
