# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Current state

Phases 0–3 and 6 are **implemented**. **Phase 4 (hardware decode) was BLOCKED on m114 but is FIXED on
M138/cobalt-27 (the current pin).** On m114, `--enable-features=VaapiVideoDecodeLinuxGL --use-angle=gl`
selected `VaapiVideoDecoder`/`kIsPlatformVideoDecoder=true` on 720p VP9 BUT painted **green-band
corruption on every frame** (ANGLE's DMA-buf import of the tiled NV12 VA surface was broken on
radeonsi); every automated metric (`corruptedVideoFrames=0`, decoder name) false-passed and a user saw
it. **On M138 (2026-07-10, OLED) the SAME two flags were re-tested on-Deck through the flatpak (Game
Mode, real Leanback VP9 playback) under the DUAL gate — `VaapiVideoDecoder`+`kIsPlatformVideoDecoder`
AND a composited screenshot through `video_corruption_verdict` — and came back clean: 0.0% green-band
rows across 5 frames, visually confirmed.** M138's newer ANGLE fixes the import, so `config/app.json`
now **ships hardware decode** (the two flags are in `cobalt_flags`). The P4 power number is now
**measured**: `just power` on-Deck (OLED, 2026-07-10) averaged **5.6 W** under HW-decode VP9 playback,
well under the ≤~9 W gate. STILL OPEN on M138: only the LCD unit (unknowable). Do
NOT trust these flags after a future Cobalt bump without re-running the PIXEL check. See
durable/hardware.md (★ RESOLVED note) and m114.md §"VA-API decode is VISUALLY CORRUPT" for the history.
"Implemented" is not "verified" — say which you mean. Verified *on hardware* (all OLED): gamepad input,
hotplug, TV UA, VP9 **hardware** decode (M138, clean) + AV1 steering, persistent
profile, deep link, audio, Game Mode launch, single-instance lock, L3 conformance (45/46, same as
workstation), and the CDP-injected controls card and toast rendering over real Leanback (they needed a
Trusted Types policy; bare `innerHTML` is a silent no-op on youtube.com/tv, m114.md).

**Touch lock is proven DEAD, not merely untested** (durable/touch-lock.md): the launcher runs as the
seat user `deck`, which cannot `open()` the FTS3528 touchscreen — udev grants `uaccess` to joysticks
only, so the panel node stays `root:input 0660` with `deck` not in `input`, and `--device=input`
inside the Flatpak changes visibility, not ownership. `EVIOCGRAB` never gets a file descriptor. The
L2 harness cannot even fake a panel gamescope will route. **Superseded 2026-07-10 by `disable_touch`
(default on):** touch is instead made inert two ways that DO work — the Navigator injects
`config/no_pointer.js` to swallow every pointer/mouse/touch event in the page (Option A), and
`launcher/src/touchmode.cpp` holds gamescope's global `STEAM_TOUCH_CLICK_MODE` at hover (0) while our
window is focused (Option B). Verified on-Deck: at hover a tap produced 45 mousemoves and 0 clicks and
did not navigate. The dead `touch_lock_*`/EVIOCGRAB machinery ships disabled. **Still** not verified on
hardware:
auto-repeat acceleration, mic capture, and voice search. **P4 power: PASSED 2026-07-10** — `just
power` averaged **5.6 W** under HW-decode VP9 playback (OLED), under the ≤~9 W gate. **P6
suspend/resume: PASSED 2026-07-10** — `just soak` ran `rtcwake` cycles with the app alive and the
video advancing across every resume, and its audio-restored and no-screen-dim clauses are now
verified too. `just soak` needs passwordless `sudo rtcwake` (a `zz-`-prefixed sudoers drop-in;
see the script's own error text). All OLED-only; the LCD unit has run nothing. See
`.internal/TEST-PLAN.md` §2 before repeating any status claim.

**The L2 suite (`just test-deck`) ran against a Deck for the first time on 2026-07-10** — it never
had before, because `reachable()` could not parse the `user@host` in `DECK_HOST` and reported every
Deck unreachable (durable/harness.md F14). Treat pre-2026-07-10 "L2" claims as never-executed.

The authoritative design doc is `.internal/steamdeck-cobalt-youtube-plan.md` —
read it in full before doing any implementation work. It defines the phased plan (P0–P10), locked
decisions, risk register, toolchain rules, container setup, and the complete `just` command surface.
This CLAUDE.md summarizes the non-obvious parts; the plan is the source of truth and wins on
conflict — **except where a registered finding contradicts it**, since findings record what the
hardware actually did. Check `.internal/TASKS.md` for real status before trusting any phase claim
here. **All agent/planning docs live under `.internal/`** (design doc,
`.internal/TASKS.md` execution checklist, `.internal/TEST-PLAN.md` test tiers + honest
tested/untested matrix, `.internal/HARNESS.md` the build/deploy/test command surface + exit-code
taxonomy, `.internal/findings/` registered findings, `.internal/MIGRATION.md`,
`.internal/configurator-plan.md` the design for the not-yet-built "Deckback Settings" companion app);
`docs/` holds public deliverables (`HOW-IT-WORKS.md` the built-state/how-it-works overview,
`SUPPORT.md`, `legal.md`).

**Before running or adding any `just` recipe, read `.internal/HARNESS.md`.** It defines the exit-code
taxonomy (`2` = the product is wrong; `3` = environment; `4` = transport — retry these, never a `2`),
which machine each recipe runs on, and which recipes still need a human. Recipes that decide pass/fail
must have L0 coverage in `tests/harness/` — `just power` once reported `mean 0.00 W … PASS` on a Deck
with no battery telemetry, because a check that cannot fail is not a check.

**Never push without `just preflight` green; never release from a commit that is not green in CI.**
`scripts/preflight.sh` (`just preflight`) is the ONE definition of the pre-push / pre-release gate —
shellcheck + the harness suite + clang-format-18 + the launcher gcc/clang builds + gn-args. Both
`.githooks/pre-push` and CI (`.github/workflows/lint.yml`) call it, so local and CI cannot drift;
`just release` refuses to build unless the tagged commit's CI is green (`FORCE=1` overrides). Run
`just hooks` once per clone — it is mandatory, not optional. This exists because three consecutive
red pushes (a stale test, unformatted C++, a clang-only `-Werror`) each slipped through a hook that
mirrored only *part* of CI. Pinned clang-format **18** matters (v20 formats differently); the
launcher build needs `libxcb1-dev`/`libpulse-dev` to match CI. See
`.internal/findings/durable/preflight-parity.md`.

**Before claiming anything works, check `.internal/TEST-PLAN.md` §2.** Unit tests here prove pure
functions; they prove nothing about Leanback, gamescope, or the GPU. The L2 (on-Deck automated) tier
does not exist yet, every hardware result so far is from the **OLED** unit, and several shipped
features have **zero** verification. New features get a failing on-device test first (§0).

**Where knowledge lives.** Register findings in `.internal/findings/`, split into `durable/`
(survives Cobalt bumps: architecture, hardware/AV1 facts, strategy, locked decisions) and
`milestones/<mNNN>.md` (version-scoped: GN args, working UA, VA-API/CDM state, patch-rebase — keyed
to `DEPS.pin`, presumed stale on a bump). The yearly Cobalt-bump procedure is
`.internal/MIGRATION.md`. Before trusting a version-specific fact, confirm which layer it's in.

## What this is

An unofficial native YouTube TV (Leanback) client for Steam Deck (LCD/Van Gogh + OLED/Sephiroth),
SteamOS 3.x, **Game Mode only**. Engine: Chromium-based Cobalt ("Chrobalt", `youtube/cobalt`
trunk, ~Chromium M114). It loads `https://www.youtube.com/tv` in a kiosk shell and adds
controller input, hardware decode, sleep/resume, mic/voice search, and Widevine (best-effort L3).

Branding constraint: never put "YouTube"/"YT" in the app id or name; app id is `io.github.<you>.<name>`.

## Architecture (the big picture)

Three cooperating layers, each with a **different toolchain** (see below):

1. **Cobalt engine** — a `gclient`-managed Chromium/Cobalt checkout, pinned by `DEPS.pin` and
   modified via a rebaseable **quilt patch series in `patches/`** (never edited in place). The
   patch surface stayed smaller than planned: gamepad input, `MediaCapabilities`/AV1 steering, and
   the mic auto-grant all landed in the launcher over CDP instead. `patches/series` holds exactly
   **two** entries: (1) Widevine CDM registration, because the shell has no
   `AddContentDecryptionModules` override and `enable_widevine` alone loads nothing (m114.md
   "Widevine registration gap") — it compiles, links, and regresses neither `just smoke` nor
   `just cert` (2026-07-10); with no CDM installed it correctly registers nothing, and
   `com.widevine.alpha` rejects with `NotSupportedError`; **no real CDM has ever been loaded** — CI
   must never fetch Google's (`docs/legal.md`); (2) `support_web_tests=false` in content/shell —
   content_shell is upstream's web-test shell and otherwise always links ~1,134 TUs of dead test
   harness (2026-07-13, part of the build-slimming pass; see
   `.internal/findings/durable/build-slimming.md`, which also documents the feature-off GN args in
   `args/common.gn`: −9.5% stripped binary, no SwiftShader, smoke-verified — on-Deck gates pending).
2. **`launcher/`** — a small standalone C++ shim (its own CMake build, no Chromium checkout
   needed): env/flags/config, evdev gamepad→CDP key injection + touchscreen lock, logind
   sleep-watcher over D-Bus, idle-inhibit manager, startup CDP policy (TV UA, AV1 steering, mic
   grant), CDM fetcher, watchdog restart.
3. **`config/`** — `app.json` (UA string, URL, quality caps, keymap — designed to be
   **remotely overridable/hot-swappable** so Leanback server-side breakage can be hotfixed without
   a rebuild) and `steam_input.vdf` controller template.

Key bets: identity is UA-driven (biggest external risk — R1); input is handled at the embedder/C++
layer, not JS injection; the app depends on Google's tolerance of the TV UA (state this openly).

## Two toolchains — do not mix

- **In-tree (`patches/`, Cobalt):** **C++17 only.** Use Chromium's hermetic bundled Clang, lld,
  and in-tree libc++ (`use_custom_libcxx=true`). Do NOT use system Clang/GCC or C++20+ — version
  skew breaks `-Werror`/plugins/LTO and leaking C++20 maximizes rebase pain. Format with the tree's
  `buildtools/linux64/clang-format`.
- **Out-of-tree (`launcher/`, tools):** **C++23**, must build cleanly on **both Clang ≥18 and
  GCC 14** (Flatpak SDK is GCC-based), `-Wall -Wextra -Werror`. Minimal deps only: libsystemd,
  libevdev, libcurl. No Boost/Folly-class dependencies.

## Build & dev workflow

Builds run on a **workstation/CI, never on the Deck**. Measured footprint for a `--no-history`
checkout + one preset: source tree 22 GB, `out/deck` 9.9 GB, sccache 787 MB, flatpak cache 3.1 GB,
docker images ~3 GB — **≈45 GB total**, not the 100–150 GB this doc used to claim. Put the tree on a
real Linux filesystem: a WSL `/mnt/*` (9p) mount is case-insensitive, drops mode bits, and is ~100×
slower at file creation — Chromium will not build correctly there.
The Deck is a deploy/test target over SSH only. All work happens inside a Docker/Podman
container based on **Debian 12 (bookworm)** — matching Cobalt trunk, which builds every image on
`marketplace.gcr.io/google/debian12` (see `docker/Dockerfile`; base is a hermetic-toolchain "tool
caddy", so glibc is required — no Alpine/musl). The `pack` stage is the deliberate exception
(`PACK_BASE=debian:13-slim`); everything that touches Chromium is bookworm. Source is bind-mounted, never baked in. The image's
package lists are mirrored from `cobalt/docker/{linux,unittest}` and must be re-synced on each
Cobalt bump (`.internal/MIGRATION.md`). `sccache` lives on a named volume shared between local and CI.

Command runner is **`just`** (every recipe delegates to `scripts/*.sh` so CI can call scripts
directly). Planned recipes — see plan §15 for the full table:

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
