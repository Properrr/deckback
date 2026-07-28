---
scope: durable
title: Feature landscape — what is worth adding for the Steam Deck, and what it would cost
created: 2026-07-28
status: research (no code, no on-Deck run) — every "unknown" below names the probe that settles it
method: read the plan, TASKS.md, the registered findings, and the launcher/config surface at ea08f9a
sources:
  - .internal/steamdeck-cobalt-youtube-plan.md (§6 phases, §10 stretch backlog, §8 risk register)
  - .internal/findings/durable/{pip,preview,input-ux,platform,keep-awake,page-scripts,touch-lock,self-update}.md
  - launcher/src/{touchmode,config,haptic}.cpp, config/app.json, config/scripts/, flatpak/io.github.properrr.deckback.yml
---

# Feature landscape

A ranked survey of features Deckback could add, written from the mechanisms this project has already
proven rather than from a wish list. Nothing here was run on a Deck. Every item states the lever it
would use, what is genuinely unknown, and the probe that decides it — so no item can be started on a
guess (the no-guessing policy that killed `c`-for-captions and chapter-via-`/player`).

Read `durable/pip.md` first if the idea sounds like "a second video". Read `input-ux.md` §8 first if
it sounds like "bind a key".

## 1. The levers we own (why some features are nearly free here)

| Lever | Proven by | Cost to reuse |
|---|---|---|
| CDP page scripts through `ScriptLibrary` (`config/scripts/*.js`) | AV1 steering, skip, chapter seek, captions, toast, OSD | ~free, hot-swappable, no rebuild (`page-scripts.md`) |
| The TVHTML5 player object on **`.html5-video-player`** | `seekTo`/`seekBy`/`getCurrentTime`/`getPlayerState`, caption `getOption`/`setOption` (input-ux §18) | free — but each *new* method needs a method probe before it is bound |
| InnerTube `/youtubei/v1/next` with the page's own TVHTML5 context | chapters via `macroMarkersListEntity`, on-Deck 2026-07-13 | free; R1-fragile, mitigated by script hot-swap |
| evdev read in the launcher → keys/actions/layers | gamepad, analog triggers, right-stick ramp | small C++, all pure logic is L0-testable |
| **X-atom writes to gamescope** | `touchmode.cpp` holds `STEAM_TOUCH_CLICK_MODE` at hover while focused | small C++, probe-gated per atom |
| Host-side helper service *outside* the sandbox | `idle-nudge`, `audio-repair` | medium: installer + self-uninstall surface |
| The OSD menu shell (checkbox/combobox widgets built, unused) | `osdmenu.cpp` + `config/scripts/osd.js` | a new tab ≈ a model builder + its L0 test |

### 1.1 The cross-cutting cost nobody guesses: new Flatpak permissions

`flatpak-portal` refuses a self-update whose new metadata asks for **any** permission the *installed*
build lacks, so adding one line to `finish-args` strands every existing user on their current version
until they reinstall by hand (`self-update.md` §"permission adds break self-update";
`scripts/check-permissions.sh` gates it in `just preflight`). A feature needing a new `--own-name` or
`--talk-name` therefore costs a migration, not a line of YAML. Price that in **before** designing.

## 2. Tier 1 — genuinely Deck-shaped, highest value

### 2.1 Audio-only / screen-off listening

The biggest handheld-specific win and the one nothing in the repo covers. Every part already exists,
used in reverse: stop the host-side idle nudge so the panel blanks, hold the logind **sleep**-block
inhibitor so Steam does not auto-suspend, and (if the player exposes it) drop to the lowest video
quality so decode work collapses.

- **Known:** the nudge is what keeps the screen alive, and it is gated on our own play-state
  inhibitor (`keep-awake.md`); the sleep-block inhibitor demonstrably stops auto-suspend.
- **Known trade-off:** that same sleep-block also blocks a *deliberate* power-button sleep — the
  reason it was shipped and retired in one session on 2026-07-11. Scoping it to the mode and
  releasing on exit is the design question, not an afterthought.
- **Unknown (probe):** does Chromium keep decoding and feeding audio with the panel blanked under
  gamescope? Play, blank, then assert `currentTime` still advances (the `deckctl.py` advance check
  already exists) and audio is audible.
- **Unknown (measure):** the actual watt delta. `just power` is closed-loop now, so this is a real
  number, not a hope.

### 2.2 Docked = a real TV box

We built a *TV interface* for the one handheld that docks to TVs, and docked is currently
"smoke only, must not crash" (plan §6 P9) with 4K docked a stretch goal (§10.2). Today the surface is
pinned `1280x800` by `--content-shell-host-window-size` in `cobalt_flags`, so a docked 1080p/4K TV
gets a gamescope upscale of an 800p buffer.

- **Unknown (probe):** what gamescope actually does to our surface when an external display is
  attached — mode, scaling, whether we are still direct-scanned-out.
- **Unknown (probe):** can the shell be resized at runtime (`Browser.setWindowBounds` may not be
  implemented in content_shell), or does this mean a watchdog-driven relaunch at the new mode? The
  restart machinery exists either way.
- Highest "this is why I would install it" value per engineering day, and it is the only idea here
  that changes what the product *is* rather than how it behaves.

### 2.3 Phone as remote and keyboard — probably zero code

Text entry is the worst structural hole in the product: no auto-OSK under Xwayland, `STEAM+X` can
soft-lock the session, BT keyboards are forced to QWERTY scancodes (input-ux §8.3, risk R9).
YouTube's own TV app ships a manual **"Link with TV code"** pairing that runs over its lounge
service — cloud-side, needing no LAN/DIAL server from us.

- **Unknown (probe, decisive):** is that pairing screen reachable in Leanback's settings **under our
  Cobalt UA**? If yes, search, sign-in and queueing all move to the phone and the deliverable is a
  `docs/SUPPORT.md` section, not code.
- Same probe shape as `preview.md` §1: our own UA is the most likely thing suppressing a feature that
  would otherwise work (the voice-search lesson, input-ux §13.2).
- If it is *not* reachable, register it as a page-layer dead end and stop — do not build a remote.

### 2.4 Touch gesture layer (in flight on `feat/touch-gestures`)

Flick → arrow bursts, left-edge swipe → Escape, double-tap → seek. Beats the Switch YouTube app's
most-criticised gap (no touch scrolling at all) and closes input-ux §3/§7.

**The price dropped since input-ux §11 was written.** That section's "gestures force a permanent
exclusive grab" argument rested on `EVIOCGRAB` working — and `touch-lock.md` proved on hardware that
it does not even apply (the launcher, as the seat user, cannot open the panel node; gamescope reads
it by a path a grab would not intercept). The live question is now gamescope's touch **mode 4**
passthrough (`wlr_seat_touch_notify_down`, real `wl_touch`, no pointer emulation) and whether its
Xwayland advertises a Direct-mode XI2 device that Chromium's `TouchFactory` picks up. `just
touch-probe` answers exactly that with a human at the panel.

## 3. Tier 2 — cheap, entirely on proven mechanisms

- **Playback speed.** `<video>.playbackRate` is certain; whether the TVHTML5 player exposes a rate
  API (and whether it overrides ours on each video) needs the same method dump that found `seekBy`.
  Bind to a freed rear grip or an LT layer; mobile's hold-for-2× is the reference model.
- **Sleep timer / "stop after this video."** An OSD tab, a player pause, an inhibitor release, and
  optionally `login1.Suspend`. Falling asleep watching in bed is a handheld-only failure mode, and
  every part of it is already wired.
- **Free the rear grips.** `config/steam_input.vdf` duplicates L4/L5/R4/R5 onto face buttons, so four
  physical controls are invisible to `input.cpp` by construction (they are not on the virtual pad —
  input-ux §1). Repointing two of them to distinct xinput buttons is a `.vdf` + keymap edit and
  unblocks speed / audio-only / captions bindings.
- **Trackpad → arrow bursts.** Same router as the gesture layer: Leanback has no pointer scroll, so
  relative motion must become arrow keys. No other YouTube client has trackpads to spend.
- **Battery-aware quality cap.** `quality.max_height` and `render.*` are currently in `config.cpp`'s
  `kIgnoredPrefixes` — declared in `app.json`, consumed by nothing. Wiring a real cap (and lowering
  it under a battery threshold) makes the config honest and adds a Deck-specific behaviour. Confirm
  first that `/sys/class/power_supply` is readable from inside the sandbox.
- **Panel refresh matching (OLED).** `platform.md` records that the QAM perf knobs are user-facing
  settings, not gamescope flags we can pass — but `touchmode.cpp` proves we can *write gamescope
  atoms*. Whether a refresh/FPS-cap atom exists and honours a non-Steam client is a probe, not a
  known no. 90 → 60 Hz during 60 fps content is free watts if it lands.
- **Stats tab in the OSD.** Decoder name + `kIsPlatformVideoDecoder`, resolution, dropped frames,
  watts. The Media-domain accumulator already collects it (`scripts/cdp.py:MediaState`), and on a
  battery device "am I actually on hardware decode" is a *user* question — the exact question whose
  silent false-pass shipped green-band corruption on m114.
- **Haptics on seek/chapter confirm.** `haptic.cpp` exists, the Deck's rumble is good, the plan
  already sanctions it (§P3, off by default).

## 4. Tier 3 — plausible, value uncertain

- **MPRIS** (plan §10.4). Costs a new `--own-name` (see §1.1), and Game Mode has no media widget: the
  only real consumer is BT headset transport buttons via `mpris-proxy`, which is not running by
  default on SteamOS. **Probe for a consumer before building the producer.**
- **Loudness compression for the Deck's quiet speakers** via a Web Audio graph on the media element.
  Attractive for late-night handheld viewing; real risk of perturbing the MSE/VA-API path. If tried,
  re-assert decoder identity afterwards — a feature that silently drops us to software decode is a
  regression wearing a feature's clothes.

## 5. Policy-blocked, not tech-blocked

**SponsorBlock / DeArrow.** Technically now small: hot-swappable scripts, a verified `seekTo`, and an
InnerTube fetch precedent. It stays out because of locked decision **A3** (Flathub acceptance and
takedown risk), not because it is hard. Making it opt-in and default-off barely changes that
calculus. If it is ever revisited, it is a *risk* decision to re-take deliberately (plan §10.6), and
this finding is not an argument for taking it.

## 6. Do not re-propose — registered dead ends

| Idea | Why it is dead | Where |
|---|---|---|
| Picture-in-Picture | shell returns `kNotSupported`, no `VideoOverlayWindow` in the shell, gamescope hosts no second top-level, Leanback yields no second stream | `pip.md` |
| Voice search as a keypress | Cobalt routes voice through a Starboard service this Chromium build does not have | input-ux §8.2, TASKS P5 |
| Touchscreen `EVIOCGRAB` lock | the seat user cannot open the panel node; the grab does not starve gamescope | `touch-lock.md` |
| Chapters from `/player` or a spoofed WEB context | TVHTML5 `/player` carries no overlays; a WEB context returns UNPLAYABLE for every id | input-ux §18 |
| Any in-sandbox idle inhibitor | logind/ScreenSaver/portal/libei all ignored by gamescope's idle timer | `keep-awake.md` |

A feature request that matches a row here gets the row, not a re-investigation.

## 7. Cheapest next step — three probes, one Deck session, no code

1. **TV-code pairing** reachable under our UA? → decides §2.3 outright.
2. **`.html5-video-player` method dump** for rate/quality APIs → decides playback speed and the
   audio-only quality drop, using the one-liner shape that already found `seekBy`.
3. **Dock a Deck** and read back what gamescope does to our surface → decides §2.2's shape.

Each is a `Runtime.evaluate` or an observation, none needs a build, and all three are the same
probe-first discipline as the touch-lock grab test and the P11 preview spike: *classify the mechanism
before writing any code that assumes one.*
