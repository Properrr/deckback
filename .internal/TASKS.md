# Deckback — Task Plan

The execution checklist for `.internal/steamdeck-cobalt-youtube-plan.md`, which wins on conflict.
A finished phase takes one or two lines. Each open item says what is missing and, where known, what
blocks it or how to verify it. For what has and has not run on hardware, see `.internal/TEST-PLAN.md` §2.

- **App:** Deckback — `io.github.properrr.deckback`
- **Target:** Steam Deck **OLED**, the only unit we have. **LCD**: best-effort and untested
  (`durable/decisions.md`). SteamOS 3.x, Game Mode only.
- **Engine:** Chromium-based Cobalt (`youtube/cobalt`, cobalt-27 / Chromium **M138**), pinned by `DEPS.pin`
- **Legend:** `[ ]` todo · `[~]` in progress · `[x]` done · `[!]` blocked/needs input

---

## Phase S — Repo scaffolding: done.

## Phase 0 — Research spikes: done. The answers still in use:

- **S0.1:** Chrobalt (Cobalt 26+) builds and runs on generic Linux, and the engine target is
  `content_shell`. Cobalt ≤25 is classic Starboard Cobalt, the wrong engine (`durable/decisions.md` D1).
- **S0.2:** the TV UA (`config/app.json:user_agent`) gives interactive Leanback, while a CrKey UA
  lands in cast-receiver mode. `content_shell` ignores `--user-agent`, so we set the UA through CDP.
- **S0.3:** the Widevine library-CDM path is intact. Only `enable_widevine` gates it.
- **S0.4:** `--ozone-platform=x11` on gamescope's Xwayland renders Leanback with a real RADV GPU process.
- **S0.5:** media goes through Chromium's pipeline, not `SbPlayer`. VA-API needs
  `--enable-features=VaapiVideoDecodeLinuxGL --use-angle=gl`, because the default ANGLE/Vulkan
  backend silently falls back to software. libva on the OLED advertises `AV1Profile0` (`durable/hardware.md`).
- **S0.6:** CDP key events are trusted, so input is launcher-side evdev→CDP with no engine patch.
  Under Steam Input, the pad we can read is the virtual Xbox 360 pad.

## Phase 1 — Build & bring-up on Deck

Done: the persistent profile (`$XDG_DATA_HOME/deckback/profile`, passed as `--data-path`) survives a
Flatpak reinstall, and the app launches in Game Mode from the Steam shortcut.

- [ ] Reproducible build: verify `just gen dev/deck` and sccache. The pin itself is set in `DEPS.pin`.
- [ ] First run in Desktop Mode (X11): no run is recorded. Game Mode launch is verified.
- [ ] Force a 1280×720 render surface and check text sharpness against 1080p-authored Leanback. Not built.
- [ ] Crashpad on, local minidumps only, no telemetry. Not built.

**Gate (not run):** cold boot → Game Mode → launch → browse → play 1080p + audio, stable for 30 min.

## Phase 2 — Session & lifecycle   → `launcher/`

Done: env, flags and config; the rotating log (`log.cpp`); watchdog restart-on-crash; deep links
(`app://<id>` → `#/watch?v=<id>`, verified on-Deck). On a fresh profile, Leanback's account gate
swallows every route until someone dismisses it. A failed navigation shows a controller-focusable
Retry page (`errorpage.cpp`), and `error_title`/`error_hint` in `app.json` are the R1 hotfix surface.

- [~] Single-instance lock (`flock`): implemented, not verified on-Deck.
- [~] Clean exit: the OSD's Y-hold (600 ms) and SIGTERM both shut down cleanly with exit 0. Steam's
      "Close game" path in Game Mode is not verified on-Deck.
- [ ] Error page on hardware: pull the network, then check that the injected page renders under
      gamescope and that an injected Enter reaches Retry (an L2 test).

**Gate (not run):** 50 launch/kill cycles through the Steam UI with zero profile corruption.

## Phase 3 — Input: PS5-parity controls   → `launcher/src/input.cpp`, `config/steam_input.vdf`

Done: evdev→CDP key injection with multi-device merge and hotplug (verified on-Deck). The keymap,
context layers (`keymap_player`/`keymap_osk`) and held-trigger modifier layers
(`keymap_lt`/`keymap_rt`) come from `config/app.json`; every layer ships empty until the key spikes
below. The OSD Settings menu (`osdmenu.cpp`, `config/scripts/osd.js`; Menu ☰, off playback; tabs
Settings ▸ Keys/Captions, Updates, About) passes `tests/deck/test_osd.py` 8/8 on the OLED.

Shipped bindings: A Enter · B Escape · X play/pause · LB/RB scrub (arrow keys) · LT/RT chapter seek
(`chapter_seek.js`, falls back to ±`skip_seconds`) · View captions (`toggle_captions.js`) · Menu
Settings · Y `toggle_gestures`. The triggers report analog `ABS_Z`/`ABS_RZ`, so LT/RT work as
shipped. The old aliases `seek_back_10`/`seek_fwd_10` still map to scrub and log a startup WARN.

Touch: `disable_touch` (default on) makes touch inert. `config/no_pointer.js` swallows pointer
events, and `touchmode.cpp` holds gamescope's touch mode at hover. The `EVIOCGRAB` touch lock does
not work and its code was removed (`durable/touch-lock.md`). Touch gestures are P12.4.

- [ ] Leanback OSK usable end-to-end. The Steam OSK is not a safe fallback: open it from the QAM,
      never with `STEAM+X`, which can raise the desktop OSK and soft-lock the session. Under
      Xwayland it never opens on its own. Spike first: what do Escape, Backspace and Delete do
      inside the OSK? `Delete` (46) is the erase candidate; Backspace probably pops the view.
- [ ] Bind the researched keys only after the §8.4 spikes. `j`/`l`/`k` are desktop watch-page
      shortcuts, and CEA codes 461/415/412/417 belong to Tizen/webOS, not Cobalt.
- [ ] On-Deck key spikes (input-ux §8.4, TEST-PLAN §4), each with negative controls (CEA `461`/`415`
      must do nothing): OSK erase (8/27/46), direct printable typing into search, transport keys
      179/178/227/228/176/177, color keys 403–406, speed and chapters.
- [ ] Text entry: decide and document the supported paths: the Leanback grid OSK via D-pad, the QAM
      keyboard, and a physical/BT keyboard (it reaches the app, but Game Mode forces QWERTY
      scancodes). The launcher only opens devices with `BTN_SOUTH`, so it has no keyboard path of its own.
- [ ] Steam Input keyboard-emission fallback (input-ux §8.3): Steam Input can bind controls to
      keyboard scancodes even for non-Steam shortcuts. Evaluate it as a CDP-failure fallback and
      per-user remap layer (scancode ≠ layout).
- [ ] `config/steam_input.vdf`: export the exact layout from Steam's configurator on-Deck. Optional
      focus-move rumble (off by default) is not built.
- [ ] Focus-trap sweep (input-ux §9): on every surface, press every direction from every focusable
      element. Focus must always move or be deliberately absorbed.
- [ ] Smallest-glyph measurement from an on-Deck screenshot: the smallest character must be ≥ 9 px
      at 1280×800 (12 px recommended).
- [ ] Back at the root: Back never quits; the deliberate exit is the OSD's Y-hold. Verify that
      Escape at Leanback's root under our UA neither closes `content_shell` nor leaves a blank shell.
- [ ] Input latency: log the time from evdev receipt to CDP ack for each key, and calibrate once
      against a 120/240 fps camera.
- [ ] Auto-repeat acceleration tuning on-Deck. This code has never run on a device.
- [ ] Right-stick fast scroll on hardware: confirm the virtual pad emits `ABS_RX`/`ABS_RY` and that
      Leanback keeps up with a 45 ms arrow cadence. Both fail safe.
- [ ] First-run card: `config/scripts/overlay.js` still uses an inline `<style>` block, which the M138
      CSP drops. Move it to `adoptedStyleSheets` + CSSOM. Then confirm on-Deck that the card paints
      styled and that focus behind it stays frozen.
- [ ] Captions: the View toggle flips CC on-Deck. Still to check on hardware: the toast and the CC
      on a captioned video during normal play, and the OSD ▸ Settings ▸ Captions save round trip.
- [ ] OSD writable settings still missing (via `user.json`): a button remap that keeps `select`/`back`
      bound and never rebinds the fixed OSD controls; hardware-decode and touchscreen toggles; reset to
      defaults (per key and all). R1 keys (`url`, `user_agent`, `cobalt_flags`, `cdm_*`) stay non-overlayable.
- [ ] Exit follow-ups: refuse Exit while an update deploy is in flight (`exit_enabled` is fixed at
      construction); teach Exit on the first-run card and Keys row (still `Menu (☰) — Settings`);
      tune the 600 ms Y-hold by hand.

**Gate:** every Leanback surface is controller-drivable, latency ≤ the PS5 app, and the OSK works.
Buttons and hotplug are verified; the OSK and latency are not measured.

## Phase 4 — Media pipeline & hardware decode   → `config/app.json:cobalt_flags`

Done: hardware decode ships. `cobalt_flags` carries `--enable-features=VaapiVideoDecodeLinuxGL` and
`--use-angle=gl`, verified on the OLED by two checks together: the decoder reports
`VaapiVideoDecoder` + `kIsPlatformVideoDecoder`, and a composited screenshot passes the green-band
pixel check (`video_corruption_verdict`). `just power` averages **5.6 W** under hardware VP9
playback (gate ≤ ~9 W). After any Cobalt bump, re-run the pixel check: the decoder name alone gives
false passes.

- [ ] Step 1: 1080p60 headroom, and the draw with software decode for comparison (`just power`;
      hardware decode is 5.6 W). For 720p60, see P12.15.
- [~] Step 2: AV1 steering (`config/av1_steering.js`, injected by `navigator.cpp`, gated by
      `quality.steer_av1_unsupported`) makes AV1 read unsupported while VP9/H.264 pass. Still to
      confirm on-Deck: YouTube serves VP9 and `kVideoDecoderName` is never `Dav1dVideoDecoder`.
- [ ] Settle the AV1 question. This blocks any decision to allow AV1. Force an AV1 stream and
      confirm `kVideoDecoderName=VaapiVideoDecoder`. The LCD half needs an LCD unit. SteamOS ships
      no `vainfo`, so query libva via ctypes (the OLED profile list is in `durable/hardware.md`).
- [ ] Sweep the QAM settings in `just power`: OLED refresh 90→60→45 Hz, framerate cap on/off, and
      TDP limit during VP9 playback. These are the only per-app power settings in Game Mode. The
      results feed the P8 battery section.
- [ ] Direct-scanout check: confirm gamescope direct-flips our fullscreen 1280×800 Xwayland surface
      during playback (debug HUD / gamescopectl). If it composites instead, find out why.
- [ ] Audio-wakeup experiment in `just power`: `--audio-buffer-size=<frames>` + `PULSE_LATENCY_MSEC`.
      Measure the change in draw and confirm A/V sync holds.
- [ ] Memory: trial `--process-per-site` (single-origin kiosk). Measure RSS and renderer count over
      a long browse + play session.

**Gate (not run):** 4 h of continuous battery playback (OLED), no thermal throttling, no A/V desync
after seek storms.

## Phase 5 — Audio & microphone

Done: audio out goes through PipeWire. SteamOS denies mute changes inside the Flatpak, so
`scripts/install-audio-repair.sh` installs a host-side service that unmutes Deckback's sink input
(verified on-Deck).

- [ ] Switching audio output (3.5 mm / Bluetooth) without a restart: not verified.
- [~] Mic auto-grant: `Browser.grantPermissions` `audioCapture` for the app origin only, gated by
      `mic_autogrant`. Not verified on-Deck: no prompt, and a live capture stream.
- [ ] Voice search: not shipped; the voice code was removed because the mic never worked on hardware.
      It cannot be a keypress here (Cobalt routes voice through a Starboard service this build lacks);
      a reimplementation clicks Leanback's soft-mic button over CDP (input-ux §13).
- [ ] V0 — Does Leanback render a soft-mic button under our UA? Probe `typeof window.h5vcc` and the
      DOM; if gated, try an `h5vcc` shim via `Page.addScriptToEvaluateOnNewDocument` (unverified). No
      button and no working shim means voice cannot be delivered on this engine; the docs must say so.
- [ ] V4 — Button choice: Y is now `toggle_gestures`. Using a rear grip needs P12.7 first, because
      the grips are not on the virtual pad.
- [ ] V5 — Prove the auto-grant: `getUserMedia` with no prompt, and a live, non-silent track through
      the Flatpak pulse socket. Check audio levels, not just that no error occurred.

**Gate:** voice search works in a normal room on the OLED, with a video playing and with a BT
headset mic, and audio output can switch without a restart.

## Phase 6 — Power: sleep, resume, idle: done.

Gate passed on the OLED: on every `just soak` resume the app was alive, the position advanced, audio
returned and the screen stayed lit. Suspend holds a logind delay inhibitor to pause and checkpoint;
resume waits for the network, then nudges `play()` (or reloads after `resume_reload_after_ms`, which
ships 0 = off). The logind idle inhibitor is only a signal; the host-side idle-nudge helper keeps the
screen lit (`durable/keep-awake.md`). `just soak` reports EPP but never fails on it (SteamOS can reset
it after resume, steamos#2383; writing it needs root).

- [ ] Tune `resume_reload_after_ms` on-Deck. This needs logind-driven sleeps (see the P9 `just soak` item).

## Phase 7 — Widevine (best-effort)   → `patches/0001-…-widevine-cdm-registration`, `launcher/src/cdm_fetcher.cpp`

Done: `enable_widevine = true` plus patch 0001, a shell-side `AddContentDecryptionModules` override
(the GN arg alone registers nothing). With no CDM installed, `org.w3.clearkey` resolves and
`com.widevine.alpha` rejects with `NotSupportedError`; `--widevine-cdm-path` is passed only when a
CDM is installed. `CdmFetcher` is opt-in: download from `cdm_url`, SHA-256 check against
`cdm_sha256`, atomic install, re-check at startup. We never bundle or redistribute the CDM, and CI
never fetches Google's.

- [~] A real CDM has never loaded: no licence exchange, no DRM frame. Loading one is a user-side
      step; `tests/deck/test_media.py::test_widevine_state` reports the result.
- [~] Fetcher source: support Google's component-updater endpoint (a CRX/zip, not a raw `.so`) and
      document it in SUPPORT.md. The direct-URL path is the MVP.
- [~] Explain the L3 resolution cap on DRM streams to the user. The messages for a missing or
      rejected CDM are done.

**Gate:** a Widevine test stream and one YouTube rental play, with a human-readable error when no
CDM is installed.

## Phase 8 — Packaging & distribution   → `flatpak/`, `scripts/`

Done: Flatpak on `org.freedesktop.Platform//25.08`, sandboxed via zypak; `finish-args` include
`--device=input` (never `--device=all`) and `--share=ipc`, with `--require-version=1.16.0`, and
`just flatpak`/`flatpak-lint.sh` fail if evdev access is dropped. The `pack` image is
`debian:13-slim` (`durable/packaging-toolchain.md`). `install.sh` installs, checks evdev access
(exit 2 if absent), runs `steamos-add-to-steam`, and pulls the runtime from Flathub. SUPPORT.md
covers controls, layout import, typing (QAM keyboard, never `STEAM+X`; no automatic OSK;
QWERTY-forced keyboards), DRM opt-in, sign-in, logs, and the glyph and automatic-OSK limits.

- [ ] Widevine as extra-data in the manifest (legal note in `docs/legal.md`). The opt-in
      `CdmFetcher` path exists (P7).
- [ ] Self-hosted runner for Chromium-scale builds with persistent sccache. Free runners stay lint-only.
- [~] Grid art from the local `just install` path. The one-line `web-install.sh` already writes the
      shortcut and artwork via `steam_shortcuts.py`.
- [ ] SUPPORT.md: Steam Input as the per-user remap path. Users rebind a control in Steam's
      configurator; `keymap` stays the hotfix layer.
- [ ] SUPPORT.md "Leaving the app": the OSD's Y-hold Exit pauses and shuts down cleanly, STEAM → Exit
      Game also shuts down cleanly, and holding STEAM + B force-quits.
- [ ] SUPPORT.md battery/performance section, after the P4 QAM sweep: OLED 60 Hz for video, framerate
      cap, optional TDP cap; the post-resume EPP reset on affected SteamOS builds; `LD_PRELOAD=""` in
      launch options if Game Mode stutters where an SSH launch does not (`durable/platform.md`).

### Self-update   (`durable/self-update.md`)

The portal update path works on-Deck. No item may add a sandbox permission: the portal refuses an
update that adds one, so it would leave installed users stuck on their current version.

Done, verified by L0 tests and CI replay only, never on a Deck:
- [x] Update traps closed: re-running `install.sh` updates (`--or-update`); a newer commit clears
      stale verdicts; a deploy with no verdict times out; a rejected `Update()` reports Failed.
- [x] U1 — Linear history: `publish-repo.sh` mirrors the live head and 2 parents, gates permissions,
      and generates bsdiff from-parent deltas (0.0.8 → 0.0.9: 4.2 MB instead of 66.7 MB).
- [x] U2 — Recall runbook (RELEASING.md §"Recall a bad release"): commit the last good tree forward
      again and set the bad GitHub release back to draft. Users can `flatpak mask` a version.
- [x] U3 — Publish-time permission gate against the live repo's metadata: exit 2 unless
      `ALLOW_PERMISSION_ADD=1`.
- [x] U4 — An update check that installs nothing: compare the published ref with `/.flatpak-info`
      `app-commit` at launch and on Check. It falls back to the portal, and
      `DECKBACK_UPDATE_REF_URL` points it at staging.
- [x] U5 — Deploy progress is shown, and progress restarts the 10-minute timeout.
- [x] U6 — **Close now** on Done, through the same graceful exit as the Exit hold.

Open:
- [ ] T1 on-Deck check (staging repo, `DECKBACK_UPDATE_REF_URL`): a newer commit is badged within
      seconds and Check installs nothing; a permission-adding commit shows the blocked text and
      re-running `install.sh` installs it; Update now shows progress, Close now exits, relaunch runs it.
- [x] T2 production check (v0.0.10, 2026-10-21): the signed *release-pages* run published
      `3147f33` on parent `7869c172` past the permission gate; the OLED Deck on 0.0.9 was offered it
      by the portal, deployed it from Settings ▸ Updates in 3 s and relaunched into 0.0.10. The
      update is a 148 KiB delta (a fresh install pulls 75.6 MB).
- [ ] U6 follow-up: spike the portal's `Spawn` with `LATEST_VERSION`. It may escape gamescope focus.
- [ ] U7 — Host-side update timer (owner decision): a `systemd --user` timer (`OnCalendar=` +
      `Persistent=true`) running `flatpak update --user -y --noninteractive`, the only way to keep the
      runtime and Mesa/VA-API current in Game Mode. Decide scope: Deckback and its runtime, or everything.
- [ ] U8 — Updatable bundles (blocked): a `--repo-url` bundle installs as a `deckback-origin` remote,
      which `channel_of_origin` reads as the DEV channel and `web-install.sh` refuses. First tell dev
      and release origins apart by URL. This helps new installs only.
- [ ] U9 — Signed `status.json`: a minisign-signed kill switch and incident banner. It shares key
      handling with signed remote config. First check that OpenSSL 3 is still in the 25.08 runtime.
- [ ] U10 — `beta` branch (needs U1). Keep `master` as stable: renaming it creates a new ref and
      forces a reinstall.
- [ ] U11 — Hosting headroom: GitHub Pages is soft-capped at 100 GB/month (≈1,300 full updates), and
      U1's deltas buy most of the headroom. Move hosting through `xa.redirect-url` only when needed.
      On R2, upload `summary` last.

Owner decisions: signing-key custody (backup, subkey/expiry, rotation via `--gpg-import`); whether
and how wide to run U7; history depth vs Pages' 1 GB; a beta channel; any telemetry or update ping;
who may trigger a recall; whether auto mode becomes user-reachable; when to move to 26.08 (25.08 is
supported to ~Aug 2027).

**Gate:** on a clean SteamOS OLED, one command takes a GitHub release to playing in Game Mode in
under 5 min.

## Phase T — Test automation & conformance   → `.internal/TEST-PLAN.md`, `tests/deck/`

Done: T1 (`scripts/cdp.py` library with `MediaState`), T2 (`tests/deck/` pytest harness, `just
test-deck`, `gate` vs `probe` markers), T3 (uinput pad over SSH), T4 (decoder-identity probe, the P4
gate), T5 (closed-loop `just soak`/`just power`), T6 (UA and AV1-steering canaries in `just smoke`),
T7a–f (self-hosted `js_mse_eme` conformance: `just cert` 45/46, the one failure `MediaElementEvents`
is a property of the suite itself, F11; not YouTube certification; Widevine never in the gate), T10
(decoder string `VaapiVideoDecoder`; Steam Input wraps a new uinput pad in a second Xbox node, merged
without duplicates).

- [x] T11 — `just deck-ci` unattended runner (deploy → launch → gate → cert → teardown, combining the
      stages' exit codes): built, but never run against a Deck.
- [ ] T8 — Chromium-native media tests against our `content_shell`: `media_unittests`, WPT
      `media-capabilities/` (`decodingInfo.any.js` covers what AV1 steering overrides) and
      `encrypted-media/`. Chrobalt no longer has `cobalt/black_box_tests`, and `starboard/nplb`
      no longer exercises real decode.
- [ ] T9 — A failing test first for every D-list feature (TEST-PLAN §4): text entry, OSK erase,
      scan keys, voice, latency, Widevine (`com.widevine.alpha` must reject while no CDM is installed).
- [ ] OSD L1 focus-model suite: next to `just smoke`, drive `osd.js` on a served page with Leanback's
      CSP and Trusted Types headers, and check computed style, not just presence. Today only
      `tests/js/osd_updates_nav.test.js` exists, on a mock DOM.

**Gate:** `just test-deck` green on a clean boot, twice. `just cert` runs unattended with zero
regressions on `conformance-test` + `msecodec-test`. No rows left in TEST-PLAN §2.C.

## Phase 9 — Test & hardening

Done: clang-format pinned at 23.1.3 (the same PyPI wheel locally and in CI); the launcher review
follow-ups (input test seam, resume hook off the logind thread, logind bus reconnect, CDP re-arm
gate, OSD Unreachable verdict, atomic CDM install, small fixes), on-Deck status in TEST-PLAN §2.B.

- [ ] Run the OLED-only matrix (doc §6 P9): playback, input, voice, power, network, account, DRM,
      update, docked smoke. There is no LCD unit (decisions.md). The code stays model-agnostic;
      only our claims narrow.
- [ ] Recruit an LCD owner for the beta. This is the only route to LCD coverage, and the schedule
      depends on it. First jobs: dump the touchscreen VID:PID, run `just cert`, and report
      `kVideoDecoderName` during playback.
- [ ] OLED touchscreen VID:PID: `touch.cpp` pins `2808:1015` (FTS3528), taken from a blog post about
      an LCD Deck. The on-Deck device listing in `durable/touch-lock.md` shows `FTS3528:00 2808:1015`,
      but `durable/hardware.md` still says unknown. Reconcile and close.
- [ ] Small beta group before Flathub, with a window for fixes.
- [ ] Measure what removing `Network.enable` saved: run `just power` against v0.0.9 on the same
      video. The 6.39 W reading on a different video cannot be compared with the 5.6 W baseline.
- [x] Pre-sleep checkpoint on real sleeps: the engine closes its DevTools socket at
      `PrepareForSleep(true)` (`session closed (end of stream)`), so the pause failed on every real
      sleep. It now retries once on a fresh session; two power-button sleeps on the OLED Deck
      (2026-10-21) logged `paused + checkpointed at 6.7s` / `14.7s before suspend`.
- [ ] `just soak` cannot exercise the launcher's sleep hooks. `rtcwake -m mem` bypasses logind, so
      no `PrepareForSleep` arrives (TEST-PLAN §1). Triggering a logind suspend over SSH needs polkit
      auth; until then, test with power-button sleeps by hand.
- [ ] `test_uinput_button_a_activates` needs a known-state fixture (TEST-PLAN §3). It expects the
      route to change, so it fails when focus is not on a tile (on the watch page Enter toggles
      play/pause), and it skips on the account picker.
- [ ] Desktop Mode without Steam Input reads the Deck's own pad wrong (hid-steam, from kernel source):
      triggers are on `ABS_HAT2Y`/`ABS_HAT2X`, `ABS_HAT0X/Y` is the left trackpad (read as a D-pad),
      and the real D-pad is `BTN_DPAD_*`, which the launcher ignores. Game Mode is unaffected; low priority.

## Phase 10 — Flathub submission & maintenance

- [ ] Flathub review (sandbox/zypak, extra-data, trademark).
- [ ] Track `youtube/cobalt` trunk monthly (`.internal/MIGRATION.md`). Keep the UA and keymap
      remotely overridable and signed for hotfixes.
- [ ] Issue templates (none in `.github/` yet). Log collection already exists (`--collect-logs`, SUPPORT.md).

## Phase 11 — Focus preview (probe first)   → `durable/preview.md`

Two questions: does Leanback's tile preview on focus/hover appear under our Cobalt UA, and does our
AV1 steering or UA suppress it? No engine blocker is known: muted autoplay is on, and there is no
cap on concurrent `<video>` elements. Write no preview code until P11.1 identifies the mechanism.

- [ ] P11.1 — Deciding spike (OLED, live `youtube.com/tv`): dismiss the account gate, then run the
      `preview.md` §1 enumeration probe before focus, on focus, and after ~3 s on the tile, and diff
      the results. Check that Leanback's own state changes, not a value we set. Classify:
      (a) animated `<img>`, (b)/(c) muted inline/MSE `<video>`, (d) nothing.
- [ ] P11.2 — Suppression A/B: re-run with `steer_av1_unsupported:false`, then under another TV UA
      (`cdp.py --ua`; `m138.md` §Leanback UA).
- [ ] P11.3 — Focus vs hover: if D-pad focus shows nothing, dispatch a CDP `mouseMoved` at the
      focused tile's centre and probe again. If the preview needs hover, the launcher needs a
      focus→`mouseMoved` synthesizer.
- [ ] P11.4 — Cost, only if a preview `<video>` appears: run the T4 decoder probe on its `playerId`,
      then measure the `just power` difference.
- [ ] P11.5 — Record the outcome in `durable/preview.md` §6: already works (document) · our config
      suppresses it (steering stays default-on, so probably no preview) · hover-gated (build the
      synthesizer) · nothing (register a page-layer dead end).

**Gate:** P11.1 classified on a clean boot, with the outcome recorded. Run it whenever the OLED is attached.

## Phase 12 — Feature backlog, probe first   → `durable/feature-landscape.md`

No item starts before a probe identifies its mechanism. Rows in the finding's §6 dead-end table are
not re-investigated. A new `finish-args` permission leaves existing users stuck, because the portal
refuses a self-update that adds one; price that in first. Everything here is OLED-only.

*P12.0: deciding probes (`just feature-probe <player|pairing|dock>`; a negative answer exits 0,
"could not observe" exits 3):*

- [ ] P12.0a — TV-code pairing: can our UA reach Leanback's "Link with TV code" screen? Deferred:
      unanswered, so it proves nothing either way. It takes one human minute on Leanback's Settings
      screen with `just feature-probe pairing`. Also check whether the Deck shows up in a signed-in
      phone's device picker with no code (the same account is now enough). The answer decides P12.3.
- [x] P12.0b — Player API during playback: rates `[0.25 … 2]`, quality ladder
      `["hd720","large","medium","small","auto"]`, `<video>.playbackRate` holds. The `set*` methods are
      only proven to exist. Run with media loaded (on the browse screen the getters return `[]`).
- [x] P12.0c — Dock: with a 1920×1200 panel attached, gamescope's screen stays 1280×800, and the TV
      shows the app mirrored and upscaled.

*Tier 1:*

- [ ] P12.1 — Audio-only / screen-off: stop the idle nudge, hold the logind sleep-block inhibitor only
      in this mode (it also blocks a deliberate power-button sleep, `keep-awake.md`), lower quality.
      Probe that audio continues with the panel blanked, then `just power`. `playAudioOnly` is an A3
      decision; Leanback's "Continue watching?" prompt ends long sessions.
- [!] P12.2 — Docked output: not something the app can fix. The external display mode never reaches
      the nested Xwayland, so no window change of ours finds more pixels. Next: probe whether Game
      Mode's display settings change gamescope's screen size. If they do not, register it as upstream.
- [ ] P12.3 — Phone as remote/keyboard: if P12.0a passes, the deliverable is a SUPPORT.md section,
      the cheapest answer to R9.
- [x] P12.4 — Touch gestures (`touch_gestures`, default off; `durable/touch-gestures.md`): a page
      router on real `wl_touch` at gamescope touch mode 4, every gesture finger-verified. `touchmode.cpp`
      re-asserts `STEAM_TOUCH_CLICK_MODE` every tick while focused, never reading first (Steam resets it).
- [x] P12.4a — Multitouch is delivered: `multiFinger` reached 5 in 48 sequences. Yet
      `navigator.maxTouchPoints` reads 0, so nothing can feature-detect touch, which is why the
      feature is behind a config switch.
- [ ] P12.4b — Pinch / two-finger gestures. First probe how many touch points arrive (raise the
      router's abandon threshold and read `maxTouches`), and whether coordinates stay sane with two
      fingers down. Pinch-to-fill is post-1.0 (input-ux §7).
- [ ] P12.4c — No test tier checks that touch input actually arrives. The L2 test checks that
      `STEAM_TOUCH_CLICK_MODE` is 4, which stays true even while touch is broken. A real check needs
      a finger or a synthetic XI2 touch device, read through the router's `stats().sequences`.

*Tier 2:*

- [ ] P12.5 — Playback speed, from `getAvailablePlaybackRates()`. Open questions: does the rate
      survive a video change, and does `setPlaybackRate` actually work? One report says speeds of
      1.25× and up force 480p, and that picking a quality resets the speed. Re-check quality after
      each change, and offer a remembered default. Needs a free control (P12.7).
- [ ] P12.6 — Sleep timer / "stop after this video": an OSD tab, a pause, and releasing the inhibitor;
      optionally `login1.Suspend`.
- [ ] P12.7 — Free the rear grips: `steam_input.vdf` maps L4/L5/R4/R5 onto face buttons, so
      `input.cpp` cannot tell them apart. Repoint two of them to distinct xinput buttons. This
      unblocks P12.5, P12.1 and V4.
- [ ] P12.8 — Trackpad → bursts of arrow keys, through the same router as P12.4 (Leanback has no
      pointer scrolling).
- [ ] P12.9 — Battery-aware quality cap: `quality.max_height`/`render.*` are parsed but unused
      (`config.cpp` `kIgnoredPrefixes`); the page side exists (`setPlaybackQuality[Range]`), and `hd720`
      may be a limit we impose (P12.15). Confirm `/sys/class/power_supply` is readable in the sandbox.
- [ ] P12.10 — Panel refresh matching (OLED): find out whether a gamescope atom for refresh rate or
      FPS cap exists and works for a non-Steam app (we already write `STEAM_TOUCH_CLICK_MODE`).
      Target: 90 → 60 Hz for 60 fps content.
- [ ] P12.11 — Stats tab in the OSD: decoder and `kIsPlatformVideoDecoder`, resolution, dropped
      frames, watts (`MediaState` already has these).
- [ ] P12.12 — Haptics when a seek or chapter jump lands (`haptic.cpp` exists; off by default).

*Tier 3: first check that something would use it:*

- [ ] P12.13 — MPRIS. It needs a new `--own-name` permission, unless the host-side helper owns the
      name and drives the app over loopback CDP. Two Decky plugins would use it; there is no
      evidence the QAM reads MPRIS.
- [ ] P12.14 — Loudness compression (Web Audio on the media element). Check the decoder identity
      again afterwards.

*Research candidates (feature-landscape §9):*

- [ ] P12.15 — 720p60: stock Chromium says yes to Cobalt's extra MIME params (`framerate`, `width`,
      `eotf`), so Leanback assumes no HFR; 720p may be the missing `getVideoContainerSizeOverride`. Probe
      `isTypeSupported('video/webm; codecs="vp9"; framerate=9999')`; fix with a page script, re-run `just power`.
- [ ] P12.16 — Long-press A → Leanback's context menu (Enter held ~500 ms). Probe: `rawKeyDown Enter`,
      hold 600 ms, then `keyUp` on a tile. Our auto-repeat on the press edge must not cancel
      Leanback's timer.
- [ ] P12.17 — Resume after a crash: a watchdog restart lands on Home. Probe whether
      `#/watch?v=<id>&t=120` starts at `currentTime ≈ 120`.
- [ ] P12.18 — `window.h5vcc` shim: first `runtime.initialDeepLink`/`onDeepLink` (hand a second launch
      to the running app), later `dial.DialServer` (friendly name without "YouTube"). Probe first:
      inject `h5vcc={}` and re-run the P11/P12 probes (Leanback may then assume Cobalt services).
- [ ] P12.19 — Pause when headphones are unplugged. The jack reports through evdev
      `SW_HEADPHONE_INSERT`. Bluetooth needs PipeWire events, BlueZ (a permission) or the host
      helper. Probe with `evtest`.
- [ ] P12.20 — SABR/attestation canary. Other Leanback clients report playback dying at ~45–60 s
      for some accounts, caused on the server. Add a signed-in playback check longer than 90 s, plus
      a toast and a SUPPORT.md entry.
- [ ] P12.21 — Steam tiles for channels and playlists via `#/browse?c=UC…` / `#/watch?list=…`.
      Verify that both routes land.
- [ ] P12.22 — Remaining time / "ends at", as a page script or a P12.11 row.

*Policy-blocked:* SponsorBlock / DeArrow are out because of locked decision A3 (Flathub and
takedown risk), not because they are hard.

**Gate:** P12.0a–c classified in `feature-landscape.md`. 0b and 0c are done; 0a needs a human on
Leanback's Settings screen. Every Tier 1/2 item that starts must trace back to a passing probe.

---

## Dependency order

P0 → P1 → P2 → P3 → P9 → P10. P4–P7 depend on P1 and feed P9. P8 starts after P1. Phase T gates P9.

## Open questions

- [ ] Build infra: the workstation as CI runner, or a rented large runner (P8 self-hosted runner)?
- [ ] Beta channel: GitHub only, or later a Flathub `beta` branch (U10)?
