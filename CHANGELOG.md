# Changelog

All notable changes to Deckback are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
aims to follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html). While the app is pre-1.0,
minor versions may include breaking changes.

## [Unreleased]

### Fixed
- **Settings ▸ Updates** no longer sits on "Checking for updates…" forever when you're already on
  the latest version. That text implied a check that never finishes; it now shows a resolved
  **"No update is currently available."** (a newer release still shows what's new and the update
  actions, unchanged).

## [0.0.5] - 2026-07-15

### Fixed
- **Settings menu no longer traps input after sleep/resume.** If you opened the menu and the Deck
  slept (or the page reloaded for any reason), the menu could linger on screen while your controller
  drove the YouTube UI behind it — or the reverse, the menu could keep swallowing input with nothing
  on screen. The launcher now continuously reconciles "menu captured" with "menu actually painted",
  so a reload can never leave the two out of sync.

### Changed
- **Settings menu (OSD).** The self-update pill/card are replaced by an in-app **Settings** menu:
  a persistent top-right button (active everywhere except during video playback) opens a
  controller-driven overlay with tabs. **Settings ▸ Keys** shows the controller hot-keys
  currently in use (read from your live config, never hardcoded). **Updates** shows whether you're
  up to date or has the changelog and actions for a new release. Open it with **Menu (☰)**; navigate
  with the **D-pad** (and the right stick to scroll), **A** to select, **B** to go back/close, and
  **L1/R1** to switch tabs. On the Updates tab, **A** on *Update now* installs (applies next launch)
  and **Y** ignores a version. An amber dot on the button flags an available update.
- **Settings ▸ Keys** now lists the **L2 / R2** chapter-seek controls (Previous / Next chapter).
  They were being dropped because the launcher didn't recognise the chapter/skip seeks as controls
  it performs itself. The Updates tab's status and changelog text is also larger and better spaced.

### Added
- **About tab** in the Settings menu: what Deckback is, its feature list, version, author, and
  project/support links — all read from the app's own store metadata (one source, so the in-app
  page can't drift from the Flathub listing).

## [0.0.4] - 2026-07-14

### Added
- **Self-update, notify-first** (`self_update_mode`: `notify` | `auto` | `off`, **default
  `notify`**). Deckback now tells you when a new version is out instead of updating behind your back.
  A small **"Update available" pill** sits in the top-right corner with a **☰** hint — open it from
  the Menu button any time — and once per version an **Update available** card shows what's new, with
  colour-coded buttons: **A** Update now (green), **B** Not now (red), **Y** Ignore this version
  (yellow). Nothing installs until you press **A**; the update then applies the next time you open
  Deckback. The pill stays out of the way during video playback. Prefer silent updates? Set
  `self_update_mode: "auto"`; `"off"` disables it and you update from Desktop Mode. Updates touch
  **only Deckback**, from its own repo — no root, no password; keeping the runtime and other apps
  current is still a separate `flatpak update`. See `.internal/findings/durable/self-update.md`.

## [0.0.3] - 2026-07-13

### Added
- **L2/R2 chapter seek**: the triggers now jump to the previous/next chapter of the playing video
  (chapter boundaries fetched from YouTube's TVHTML5 `/next` endpoint and cached per video). Videos
  without chapters — or a press before the chapter data has arrived — fall back to the fixed
  ±`skip_seconds` jump introduced alongside (configurable in `app.json`).

### Fixed
- Launcher CDP requests no longer all time out instantly once the Deck has been up for ~25 days
  (a 32-bit monotonic-clock truncation in the DevTools socket deadline).

### Changed
- Slimmed the engine build: ~40 feature-off GN args (WebGPU/Dawn backends, SwiftShader, Vulkan, VR,
  TFLite/WebNN, printing/PDF/plugins, chromoting, WebRTC call codecs, HLS/MPEG2-TS, HSTS preload
  list, qt/gtk shims) plus a patch dropping content_shell's web-test harness. Stripped engine binary
  193.4 → 175.0 MB (−9.5%); `libvk_swiftshader.so` (29.4 MB) no longer ships; ~11% fewer compile
  units per build. Playback-critical paths (VP9/H.264/HEVC, software-VP9 fallback, AV1 steering,
  VA-API, Widevine hooks, voice-search mic capture) are unchanged; verified by the headless Leanback
  smoke gate. On-Deck re-verification of hardware decode is pending
  (`.internal/findings/durable/build-slimming.md`).

### Development
- **CI/local parity gate (`just preflight`)**: one script (`scripts/preflight.sh`) now defines the
  pre-push and pre-release checks — shellcheck, the harness suite, clang-format-18, and the launcher
  gcc/clang builds — and both `.githooks/pre-push` and CI (`.github/workflows/lint.yml`) call it, so
  a local push and a CI run can no longer diverge. `just release` refuses to build unless the tagged
  commit is green in CI. Run `just hooks` once per clone. See
  `.internal/findings/durable/preflight-parity.md`.

## [0.0.2] - 2026-07-10

### Added
- Optional host-side **idle-nudge helper** (`just idle-nudge`): keeps the screen on and prevents
  auto-suspend while a video plays — SteamOS Game Mode otherwise blanks and suspends it, and nothing
  inside the Flatpak sandbox can stop that. Gated on playback (menus still sleep) and doesn't block a
  deliberate power-button sleep. Self-uninstalls when Deckback is removed.
- Self-hosted Flatpak repo published to GitHub Pages, with one-click install via a `.flatpakref` and
  automatic background updates (`flatpak update`).
- Flathub submission scaffold under `flathub/` (extra-data engine manifest, `apply_extra`,
  `flathub.json`, `SUBMISSION.md`, and `just flathub-prep`).
- Steam library artwork (capsule / hero / logo / header / icon) and `scripts/steam_shortcuts.py` to
  install it onto the non-Steam shortcut.
- Neutral synthetic screenshots and Flathub-grade AppStream metadata (screenshots, branding,
  keywords) in the catalogue entry.

### Fixed
- Swapped **X/Y face buttons**: physical **X** now play/pauses. The control table used the positional
  `BTN_NORTH`/`BTN_WEST` codes, which are aliased to the labelled `BTN_X`/`BTN_Y` the Deck's pad
  actually reports — so X and Y were reversed (pressing Y did play/pause).

## [0.0.1] - 2026-07-08

### Added
- First Game Mode packaging: launches straight to the interactive TV app on a real Steam Deck.
- Controller input, persistent sign-in, sleep/resume, and audio (all verified on-Deck, OLED).
- Hardware VP9 decode via VA-API (clean on M138 / cobalt-27).
- zypak-sandboxed Flatpak with a `.desktop` entry, icon, and AppStream metainfo.

[Unreleased]: https://github.com/properrr/deckback/compare/v0.0.5...HEAD
[0.0.5]: https://github.com/properrr/deckback/releases/tag/v0.0.5
[0.0.4]: https://github.com/properrr/deckback/releases/tag/v0.0.4
[0.0.3]: https://github.com/properrr/deckback/releases/tag/v0.0.3
[0.0.2]: https://github.com/properrr/deckback/releases/tag/v0.0.2
[0.0.1]: https://github.com/properrr/deckback/releases/tag/v0.0.1
