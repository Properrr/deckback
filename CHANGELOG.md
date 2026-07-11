# Changelog

All notable changes to Deckback are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
aims to follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html). While the app is pre-1.0,
minor versions may include breaking changes.

## [Unreleased]

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

[Unreleased]: https://github.com/properrr/deckback/compare/v0.0.2...HEAD
[0.0.2]: https://github.com/properrr/deckback/releases/tag/v0.0.2
[0.0.1]: https://github.com/properrr/deckback/releases/tag/v0.0.1
