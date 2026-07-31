# Changelog

All notable changes to Deckback are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
aims to follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html). While the app is pre-1.0,
minor versions may include breaking changes.

## [Unreleased]

## [0.0.9] - 2026-07-31

### Added
- **Touch gestures.** The touchscreen can now drive Deckback like a phone. It stays **off** unless
  you turn it on — set `"disable_touch": false` and `"touch_gestures": true` in `app.json`.
  - **Swipe** to move the selection — one move per swipe, however far you swipe.
  - **Tap** to select: activate the highlighted video, or play/pause.
  - **Double-tap** the left or right third to seek 10 seconds. Keep double-tapping the same side and
    the jump grows — 20, 30, 40 seconds — instead of nudging you 10 at a time.
  - **Hold** the left third for slow motion (0.5x) or the right third for fast (2x). Let go and you
    return to the speed you were watching at, not to normal.
  - **Swipe in from the left edge** to go back.
  - Every seek and hold shows an indicator on the side you touched, so a tap is never a guess.
  - **Y — or the R4 grip — turns gestures on and off at any time**, with a toast and a rumble. The
    moment you want touch off is usually the moment a palm is already on the screen.
- **Settings ▸ Keys now lists the touch gestures** when they are on, and the Y button that toggles
  them. They are otherwise undiscoverable: nothing is drawn on the panel to hint at them.
- **A fuller feature list** in the About panel and the README.

### Fixed
- **Deckback would not install over itself if a previous install had been interrupted.** The
  installer checked the internal name Flatpak gives a sideloaded build, and Flatpak renames it when
  a leftover from an interrupted install is still present. The install had actually succeeded; only
  the check failed — and it hid the real reason behind "run it by hand to see why".

## [0.0.8] - 2026-07-24

### Fixed
- **Updating from inside the app works again.** On 0.0.7 the update would appear, "Update now"
  would say it had worked, and the next launch would still be on the old version — with the app
  then reporting no updates. 0.0.7 asked for one extra system permission, and Flatpak refuses to
  install an update that wants more access than the version you already have. That permission is
  gone, so 0.0.6 and 0.0.7 can both update to this release normally.
- **Deckback no longer claims an update succeeded before it has.** The confirmation appeared the
  instant you pressed the button, whatever happened afterwards. It now waits for the real answer
  and tells you which one it got: installed, already up to date, or failed and why. If an update
  ever is refused for needing new permissions, Deckback now says so and gives you the one command
  that fixes it.
- **"No update is currently available" is gone.** Deckback never checked at startup — it waits for
  the system to announce one, which happens about every half hour — so that line was a guess
  presented as a fact, and read as "your update already installed". It now says what it actually
  knows.

### Added
- **Check for updates.** A button in **Settings ▸ Updates** that asks right now instead of waiting
  for the next automatic check, and tells you if you are already on the latest version.
- **The changelog is readable.** Release notes were cut off after a few lines with no way to scroll
  them. In the Updates tab, **↑/↓ now scroll the notes** and **←/→ move between the buttons**; the
  hint bar at the bottom says so. Deckback also shows what changed in the version you are running,
  not only in one you have not installed yet.

## [0.0.7] - 2026-07-23

### Added
- **Closed captions on the View (⧉) button.** Press View during a video to turn subtitles on or off.
  The new **Settings ▸ Captions** section lets you list the languages you prefer, in order, and
  choose whether to favour human-written or auto-generated tracks — Deckback then picks the best
  match for each video, and translates when your language isn't offered.
- **Exit without leaving the app.** Open the menu and **hold Y** to quit, from any tab and wherever
  the highlight happens to be. Deckback pauses and saves your place first, so the next launch picks
  up where you stopped. (Holding STEAM + B still force-quits, but that skips the save.)
- **The Settings menu now opens while a video is playing.** The Menu (☰) button previously did
  nothing on the watch screen.
- **One-line install.** Deckback can now be installed with a single command in Desktop Mode — see
  the README.
- **One-step uninstall.** A single script removes Deckback along with everything it put on the
  system: both helper services and the Steam library shortcut. Your YouTube sign-in is kept unless
  you ask for a full wipe.

### Fixed
- **The screen no longer dims, and the Deck no longer suspends, in the middle of a video.** The
  helper that keeps the screen awake could quietly disable itself while Deckback was being updated,
  and then stay off — with nothing on screen to say so. It now waits much longer before deciding
  Deckback is really gone, and its own cleanup can no longer be interrupted half-finished. If the
  helper is not running, Deckback now tells you the first time you play something instead of
  leaving you to guess.
- **Long on-screen messages are no longer cut off.** Notices wider than the screen were clipped at
  both edges, with no "…" to show anything was missing. They now wrap.
- **Updates keep working after the Deck sleeps.** The update check could go silently inert after a
  suspend/resume and never recover until the app was restarted.

### Changed
- **The audio and keep-awake helpers now remove themselves** when you uninstall Deckback, instead of
  staying enabled on the system afterwards.

## [0.0.6] - 2026-07-15

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

[Unreleased]: https://github.com/properrr/deckback/compare/v0.0.9...HEAD
[0.0.9]: https://github.com/properrr/deckback/releases/tag/v0.0.9
[0.0.8]: https://github.com/properrr/deckback/releases/tag/v0.0.8
[0.0.7]: https://github.com/properrr/deckback/releases/tag/v0.0.7
[0.0.6]: https://github.com/properrr/deckback/releases/tag/v0.0.6
[0.0.5]: https://github.com/properrr/deckback/releases/tag/v0.0.5
[0.0.4]: https://github.com/properrr/deckback/releases/tag/v0.0.4
[0.0.3]: https://github.com/properrr/deckback/releases/tag/v0.0.3
[0.0.2]: https://github.com/properrr/deckback/releases/tag/v0.0.2
[0.0.1]: https://github.com/properrr/deckback/releases/tag/v0.0.1
