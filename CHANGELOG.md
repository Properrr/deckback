# Changelog

All notable changes to Deckback are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
aims to follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html). While the app is pre-1.0,
minor versions may include breaking changes.

## [Unreleased]

### Added
- **Self-update** (`self_update_mode`: `notify` | `auto` | `off`, **default `notify`**): updates are
  **detected but never enforced without consent**. In `notify` mode the launcher watches the Flatpak
  portal for a newer commit and surfaces it painlessly — a small amber dot pinned to the corner and,
  once per new version, an "Update available" card showing the version and the changelog (fetched
  from this repo's GitHub Releases). Nothing installs until you choose **Update now** (A); **Not now**
  (B) keeps the dot, **Ignore this version** (Y) hides the dot until a newer release, and the update
  is reachable any time from the **Menu (☰)**. The card and dot are launcher-drawn overlays (like the
  controls card), so a YouTube frontend change can't break them. If you'd rather it update silently,
  set `self_update_mode: "auto"`; `off` disables it entirely and you update from Desktop Mode. In
  every mode the portal updates **only Deckback**, from its own `deckback` remote — no root, no
  password — and the new version applies on the next launch; keeping the runtime and other apps
  current is still a separate `flatpak update`. Backed by an sd-bus session-bus client to
  `org.freedesktop.portal.Flatpak`; because SteamOS Game Mode has no Access-portal backend for the
  portal's consent dialog, the launcher pre-records the consent in the permission store
  (`flatpak`/`updates`=`yes`), respecting an explicit host-side `no`. The legacy boolean
  `self_update` still parses (`true`→`auto`, `false`→`off`). Also: `--version`/startup report the
  real version (compiled in from `VERSION`), and `--selftest-update` / `--selftest-deploy[-seed]`
  probe the portal. See `.internal/findings/durable/self-update.md`.

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

[Unreleased]: https://github.com/properrr/deckback/compare/v0.0.3...HEAD
[0.0.3]: https://github.com/properrr/deckback/releases/tag/v0.0.3
[0.0.2]: https://github.com/properrr/deckback/releases/tag/v0.0.2
[0.0.1]: https://github.com/properrr/deckback/releases/tag/v0.0.1
