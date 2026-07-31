---
scope: durable
title: Feature landscape — what is worth adding for the Steam Deck, and what it would cost
created: 2026-07-28
updated: 2026-07-31 — P12.0b and P12.0c RUN on hardware (OLED); see §8. P12.0a still unrun.
status: research, partly measured — §8 records what the Deck actually answered; everything else is
        still a named probe, not a result
method: read the plan, TASKS.md, the registered findings, and the launcher/config surface at ea08f9a;
        §8 is `just feature-probe {player,dock}` against a docked OLED unit, 2026-07-31
sources:
  - .internal/steamdeck-cobalt-youtube-plan.md (§6 phases, §10 stretch backlog, §8 risk register)
  - .internal/findings/durable/{pip,preview,input-ux,platform,keep-awake,page-scripts,touch-lock,self-update}.md
  - launcher/src/{touchmode,config,haptic}.cpp, config/app.json, config/scripts/, flatpak/io.github.properrr.deckback.yml
---

# Feature landscape

A ranked survey of features Deckback could add, written from the mechanisms this project has already
proven rather than from a wish list. **Written before any of it was run on a Deck; §8 records what
the hardware later said, and it contradicts parts of what follows — §8 wins.** Every item states the lever it
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

### 2.4 Touch gesture layer — ✅ BUILT AND SHIPPED (2026-07-31)

> **Done. See `durable/touch-gestures.md`.** Mode 4 delivers real `wl_touch` to Blink, so the router
> is a page-layer capture listener needing no privileges — `input-ux §11`'s exclusive-grab price
> never applied. Swipe / tap / double-tap-to-seek (accumulating) / hold-for-slow-or-fast /
> left-edge-swipe Back, with an on-screen indicator and a Y-or-R4 toggle. Ships **off** by default:
> one OLED unit, and `navigator.maxTouchPoints` reads 0 even where touch works, so nothing can
> feature-detect it. **Multitouch is delivered too** (§8.4), which unblocks pinch once a probe says
> how many points arrive.

The original entry, kept because its reasoning is why the work was attempted: flick → arrow bursts,
left-edge swipe → Escape, double-tap → seek. Beats the Switch YouTube app's most-criticised gap (no
touch scrolling at all) and closes input-ux §3/§7. The price dropped since input-ux §11 was written —
that section's "gestures force a permanent exclusive grab" argument rested on `EVIOCGRAB` working,
and `touch-lock.md` proved on hardware that it does not even apply.

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

## 8. What the Deck actually answered (2026-07-31, OLED, `just feature-probe`)

Two of §7's three probes were run on hardware. **P12.0a was not** — every attempt captured the player
or the browse feed, and the probe refuses to answer from either (see §8.3).

### 8.1 P12.0b — the player exposes BOTH rate and quality  ✅ decisive

Measured during real playback (`#/watch`, 1280x720 decoded, `duration` 4649s). The same dump taken on
the browse screen is worthless and looks identical to a negative — every quality getter answers `[]`
or `"unknown"` because nothing is loaded — so the probe now refuses to conclude unless media is
loaded (`video_is_loaded()`, pinned by an L0 test).

| Call | Answer |
|---|---|
| `getAvailablePlaybackRates()` | `[0.25, 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2]` |
| `getPlaybackRate()` | `1` |
| `setPlaybackRate` | exists (throws with no args) |
| `getAvailableQualityLevels()` | `["hd720", "large", "medium", "small", "auto"]` |
| `getAvailableQualityLabels()` | `["720p", "480p", "360p", "240p"]` |
| `getPlaybackQuality()` / `getMaxPlaybackQuality()` | `"hd720"` / `"hd720"` |
| `getUserPlaybackQualityPreference()` | `"hd720"` |
| `setPlaybackQuality`, `setPlaybackQualityRange` | exist (throw with no args) |
| `<video>.playbackRate = 1.25` | **HELD** through 3 s of real playback |

**Unblocks P12.5** (two independent levers: the element alone, or the player API), **the quality-drop
half of P12.1**, and **the page-side half of P12.9** — the ladder to walk down is real and tops out
at `hd720`, which matches our 1280x800 surface.

Not established: that the `set*` methods actually *work*. They were only proven to **exist** (a
no-arg call throws, which is the signal). Call them with real arguments before designing on them.

### 8.2 P12.0c — the external mode never reaches us  ✅ decisive, and it kills the assumed design

Docked over USB-C to a 1920x1200 panel, with the app in the foreground:

```
DP-1                connected, enabled, advertises 1920x1200
gamescope screen    1280x800  ->  1280x800     (unchanged)
our page            1279x799  ->  1279x799     (unchanged)
:1 xrandr           gamescope connected 1280x800+0+0 — no mode above 1280x800 offered
TV physically shows the app, mirrored and upscaled  (confirmed by eye)
```

**Both options §2.2 proposed are dead.** There is no runtime resize (`Browser.setWindowBounds` has
nothing larger to resize into) and no relaunch-at-the-new-mode (a restart finds the same 1280x800
screen). The external mode never reaches the nested Xwayland at all, so **P12.2 is a session /
compositor question — how gamescope is started and how SteamOS switches outputs when docked — and not
an app-side change.** That is the same wall as `platform.md`'s QAM finding: those are user-facing
settings, not flags we can pass.

Docked output is therefore **not broken today** — the TV shows the app — it is merely 800p upscaled
to a 1200p panel. Re-rank P12.2 accordingly: the "highest value per engineering day" claim in §2.2
rested on us being able to change the output size, and we cannot.

### 8.3 What three failed attempts at P12.0a taught the harness

The pairing probe printed a confident **"NO PAIRING ENTRY — register a page-layer dead end"** twice
against screens that could never have shown one, and each fix exposed the next:

1. the **watch screen** (28 chars of innerText) — "no text" and "no pairing" are the same zero;
2. the same screen once **aria-labels** were collected, whose 32 icon labels pushed it past any
   length threshold — so *length* is not the test;
3. the **browse feed** (1513 chars at `#/`) — substantive, not the player, and still not where a
   pairing row lives.

A negative now requires ≥2 settings-screen markers (`Linked devices`, `Restricted Mode`, `Privacy`,
`Sign out`, …) on the captured screen; the positive is never gated, because seeing the phrase is
seeing it. All three real captures are L0 fixtures in `tests/harness/test_feature_probe.py`.

One real signal, not a verdict: `IS_MDX_INITIALIZED` and `client-screen-nonce-store` are present in
the TV app's config under our Cobalt UA. MDX is the lounge subsystem TV-code pairing rides on, so the
build knows about pairing — which says nothing about whether a *user* can reach the screen.

### 8.4 P12.4 — RESOLVED the same day: touch works, and the capability query lies

`navigator.maxTouchPoints == 0`, `'ontouchstart' in window == false`, `(pointer: coarse) == false` —
Chromium found no Direct-mode XI2 touch device. **And touch is delivered anyway.** At mode 4, 9 taps
produced 9 `touchstart` + 9 `touchend` with per-finger ids and page-space coordinates, and the
gesture layer built on that has been driven by a finger through every gesture
(`durable/touch-gestures.md`). Multitouch too: 5 multi-finger sequences in 48.

**The lesson generalises past touch.** Every natural feature-detection here — `maxTouchPoints`,
`'ontouchstart' in window`, `pointer: coarse` — returns the wrong answer on a device where the
feature works. Anything gated on them would disable itself exactly where it should run, which is why
the gesture layer is config-gated rather than detected.
