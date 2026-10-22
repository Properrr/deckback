---
scope: durable
title: Feature landscape — what is worth adding for the Steam Deck, and what it would cost
status: research, partly measured — §8 records what the Deck answered (OLED, `just feature-probe`);
        everything else is a named probe, not a result
sources:
  - .internal/steamdeck-cobalt-youtube-plan.md (§6 phases, §10 stretch backlog, §8 risk register)
  - .internal/findings/durable/{pip,preview,input-ux,platform,keep-awake,page-scripts,touch-lock,touch-gestures,self-update}.md
  - launcher/src/{touchmode,config,haptic}.cpp, config/app.json, config/scripts/, flatpak/io.github.properrr.deckback.yml
---

# Feature landscape

A ranked survey of features Deckback could add, built from mechanisms this project has already
proven. Every item states the lever it would use, what is genuinely unknown, and the probe that
decides it — no item is started on a guess. Tasks: TASKS P12.

Read `durable/pip.md` first if the idea sounds like "a second video". Read `input-ux.md` §8 first if
it sounds like "bind a key".

## 1. The levers we own (why some features are nearly free here)

| Lever | Proven by | Cost to reuse |
|---|---|---|
| CDP page scripts through `ScriptLibrary` (`config/scripts/*.js`) | AV1 steering, skip, chapter seek, captions, toast, OSD | ~free, hot-swappable, no rebuild (`page-scripts.md`) |
| The TVHTML5 player object on **`.html5-video-player`** | `seekTo`/`seekBy`/`getCurrentTime`/`getPlayerState`, caption `getOption`/`setOption` (input-ux §18), rate + quality API (§8.1) | free — but each *new* method needs a method probe before it is bound |
| InnerTube `/youtubei/v1/next` with the page's own TVHTML5 context | chapters via `macroMarkersListEntity` | free; R1-fragile, mitigated by script hot-swap |
| evdev read in the launcher → keys/actions/layers | gamepad, analog triggers, right-stick ramp | small C++, all pure logic is L0-testable |
| Page-layer touch router at gamescope touch mode 4 | touch gestures (`touch-gestures.md`) | a capture-phase script + launcher poll → trusted keys |
| **X-atom writes to gamescope** | `touchmode.cpp` holds `STEAM_TOUCH_CLICK_MODE` while focused | small C++, probe-gated per atom |
| Host-side helper service *outside* the sandbox | `idle-nudge`, `audio-repair` | medium: installer + self-uninstall surface |
| The OSD menu shell | `osdmenu.cpp` + `config/scripts/osd.js` | a new tab ≈ a model builder + its L0 test |

### 1.1 The cross-cutting cost nobody guesses: new Flatpak permissions

`flatpak-portal` refuses a self-update whose new metadata asks for **any** permission the *installed*
build lacks, so adding one line to `finish-args` strands every existing user on their current version
until they reinstall by hand (`self-update.md` §"permission adds break self-update";
`scripts/check-permissions.sh` gates it in `just preflight`). A feature needing a new `--own-name` or
`--talk-name` costs a migration, not a line of YAML. Price that in **before** designing.

## 2. Tier 1 — genuinely Deck-shaped, highest value

### 2.1 Audio-only / screen-off listening (P12.1)

The biggest handheld-specific win. Every part exists, used in reverse: stop the host-side idle nudge
so the panel blanks, hold the logind **sleep**-block inhibitor so Steam does not auto-suspend, and
drop to the lowest video quality so decode work collapses (the player exposes a quality API, §8.1).

- **Known:** the nudge is what keeps the screen alive, gated on our play-state inhibitor
  (`keep-awake.md`); the sleep-block inhibitor stops auto-suspend.
- **Known trade-off:** the sleep-block also blocks a *deliberate* power-button sleep. Scope it to the
  mode and release on exit.
- **Policy:** a native audio-only player flag exists, but using it rewrites the player response — an
  owner decision under A3, not an engineering gap.
- **Unknown (probe):** does Chromium keep decoding and feeding audio with the panel blanked under
  gamescope? Play, blank, assert `currentTime` advances (`deckctl.py`) and audio is audible.
- **Unknown (measure):** the watt delta, with the closed-loop `just power`.

### 2.2 Docked = a real TV box (P12.2 — not an app-side item)

Docked, the external mode never reaches the app (§8.2): gamescope's screen stays 1280×800 and the TV
shows the app mirrored and upscaled. There is nothing to resize into at runtime and a relaunch finds
the same screen, so this is a session/compositor question (how gamescope is started, how SteamOS
switches outputs) — the same wall as `platform.md`'s QAM finding. Docked output is not broken; it is
800p on a larger panel.

- **Next probe:** do Game Mode's own display settings change gamescope's screen size at all? If not,
  register it as upstream.
- SteamOS 3.8 claims "better scaling on TVs" [V secondary]: re-probe §8.2 on 3.8.x and record the
  SteamOS build next to the reading; do not reopen P12.2 on that alone.

### 2.3 Phone as remote and keyboard — probably zero code (P12.3)

Text entry is the worst structural hole: no auto-OSK under Xwayland, `STEAM+X` can soft-lock the
session, BT keyboards are forced to QWERTY scancodes (input-ux §8.3, risk R9). YouTube's TV app ships
a manual **"Link with TV code"** pairing over its lounge service — cloud-side, no LAN/DIAL server
from us.

- **Unknown (probe, decisive — P12.0a, deferred):** is that pairing screen reachable in Leanback's
  settings **under our Cobalt UA**? The probe is built (`just feature-probe pairing`, rules in §8.3);
  it needs a human to put Leanback on its Settings screen. No answer yet — do not cite its absence as
  evidence either way.
- If yes, search, sign-in and queueing move to the phone and the deliverable is a
  `docs/SUPPORT.md` section. If no, register a page-layer dead end and stop — do not build a remote.
- Same probe shape as `preview.md` §1: our own UA is the most likely thing suppressing a feature
  (input-ux §13.2). `IS_MDX_INITIALIZED` is present in the TV app's config under our UA — suggestive
  (MDX is the lounge subsystem pairing rides on), decides nothing.

### 2.4 Touch gesture layer — ✅ shipped, default off (P12.4)

See `durable/touch-gestures.md`. At gamescope touch mode 4, real `wl_touch` reaches Blink, so the
router is a page-layer capture listener needing no privileges. Swipe / tap / accumulating
double-tap seek / hold-for-0.5×-or-2× / left-edge-swipe Back, an on-screen indicator, and a Y (= R4
grip) toggle. Ships **off**: one OLED unit, and `navigator.maxTouchPoints` reads 0 even where touch
works (§8.4). Multitouch is delivered; pinch (P12.4b) waits on a probe of how many points arrive.

## 3. Tier 2 — cheap, entirely on proven mechanisms

- **Playback speed (P12.5).** Two independent levers exist (§8.1): `<video>.playbackRate` held at
  1.25 through real playback, and the player's rate API advertises `[0.25 … 2]`. The touch hold
  gesture already uses rates; a controller binding (freed rear grip or an LT layer) is still open.
- **Sleep timer / "stop after this video" (P12.6).** An OSD tab, a player pause, an inhibitor
  release, optionally `login1.Suspend`. Every part is already wired.
- **Free the rear grips (P12.7).** `config/steam_input.vdf` duplicates L4/L5/R4/R5 onto face buttons
  (R4 → Y), so the launcher cannot tell a grip from its face button (input-ux §1). Repointing them to
  distinct xinput buttons is a `.vdf` + keymap edit and unblocks speed / audio-only / captions
  bindings.
- **Trackpad → arrow bursts (P12.8).** Same model as the gesture router: Leanback has no pointer
  scroll, so relative motion must become arrow keys.
- **Battery-aware quality cap (P12.9).** `quality.max_height` and `render.*` sit in `config.cpp`'s
  `kIgnoredPrefixes` — declared in `app.json`, consumed by nothing. The page-side ladder exists
  (§8.1). Confirm first that `/sys/class/power_supply` is readable from inside the sandbox.
- **Panel refresh matching, OLED (P12.10).** The QAM perf knobs are user-facing settings
  (`platform.md`), but `touchmode.cpp` proves we can *write gamescope atoms*. Whether a refresh/FPS
  atom exists and honours a non-Steam client is a probe. 90 → 60 Hz during 60 fps content is free
  watts if it lands.
- **Stats tab in the OSD (P12.11).** Decoder name + `kIsPlatformVideoDecoder`, resolution, dropped
  frames, watts. The Media-domain accumulator already collects it (`scripts/cdp.py:MediaState`).
  "Am I on hardware decode" is a *user* question on a battery device — and decoder name alone is not
  the answer (`hardware.md`).
- **Haptics on seek/chapter confirm (P12.12).** `haptic.cpp` exists; plan §P3 sanctions it, off by
  default.

## 4. Tier 3 — plausible, value uncertain

- **MPRIS (P12.13, plan §10.4).** Costs a new `--own-name` (§1.1), and Game Mode has no media widget.
  Consumers: BT headset transport buttons via `mpris-proxy` (not running by default on SteamOS), and
  two Decky plugins (MusicControl ~76k, Music Control ~38k downloads, store API 2026-10-20) [V].
  Probe for a consumer before building the producer.
- **Loudness compression (P12.14)** for the Deck's quiet speakers via a Web Audio graph on the media
  element. Real risk of perturbing the MSE/VA-API path; if tried, re-assert decoder identity
  afterwards.

## 5. Policy-blocked, not tech-blocked

**SponsorBlock / DeArrow.** Technically small: hot-swappable scripts, a verified `seekTo`, and an
InnerTube fetch precedent. It stays out because of locked decision **A3** (Flathub acceptance and
takedown risk). Opt-in/default-off barely changes that. Revisiting it is a *risk* decision to re-take
deliberately (plan §10.6); this finding is not an argument for it.

## 6. Do not re-propose — registered dead ends

| Idea | Why it is dead | Where |
|---|---|---|
| Picture-in-Picture | shell returns `kNotSupported`, no `VideoOverlayWindow` in the shell, gamescope hosts no second top-level, Leanback yields no second stream | `pip.md` |
| Voice search as a keypress | Cobalt routes voice through a Starboard service this Chromium build does not have | input-ux §8.2 |
| Touchscreen `EVIOCGRAB` lock | the seat user cannot open the panel node; the grab does not starve gamescope | `touch-lock.md` |
| Chapters from `/player` or a spoofed WEB context | TVHTML5 `/player` carries no overlays; a WEB context returns UNPLAYABLE for every id | input-ux §18 |
| Any in-sandbox idle inhibitor | logind/ScreenSaver/portal/libei all ignored by gamescope's idle timer | `keep-awake.md` |
| App-side docked resolution | the external mode never reaches the nested Xwayland | §8.2 |

A feature request that matches a row here gets the row, not a re-investigation.

## 7. Probes (`just feature-probe <player|pairing|dock>`)

| Probe | Decides | Status |
|---|---|---|
| P12.0a TV-code pairing reachable under our UA | §2.3 | **not run** (deferred) |
| P12.0b `.html5-video-player` method dump | playback speed, the audio-only quality drop, P12.9's page side | run — §8.1 |
| P12.0c dock a Deck, read gamescope's surface | §2.2 | run — §8.2 |

Each is a `Runtime.evaluate` or an observation; none needs a build. A negative answer exits 0 (it is
a finding); only "could not observe" is an error (3). *Classify the mechanism before writing code
that assumes one.*

## 8. What the Deck answered (2026-07-31, OLED, `just feature-probe`)

### 8.1 P12.0b — the player exposes BOTH rate and quality  ✅ decisive

Measured during real playback (`#/watch`, 1280x720 decoded). On the browse screen every quality
getter answers `[]` or `"unknown"` — indistinguishable from "no API" — so the probe refuses to
conclude unless media is loaded (`video_is_loaded()`, L0-tested).

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

Unblocks P12.5, the quality-drop half of P12.1, and the page-side half of P12.9. The `set*` methods
are proven only to **exist** — call them with real arguments before designing on them. The `hd720`
top is not proof that a cap can only lower quality: see §9.3.

### 8.2 P12.0c — the external mode never reaches us  ✅ decisive

Docked over USB-C to a 1920x1200 panel, app in the foreground:

```
DP-1                connected, enabled, advertises 1920x1200
gamescope screen    1280x800  ->  1280x800     (unchanged)
our page            1279x799  ->  1279x799     (unchanged)
:1 xrandr           gamescope connected 1280x800+0+0 — no mode above 1280x800 offered
TV physically shows the app, mirrored and upscaled  (confirmed by eye)
```

No runtime resize (`Browser.setWindowBounds` has nothing larger to resize into) and no
relaunch-at-the-new-mode (a restart finds the same screen). P12.2 is a session/compositor question,
not an app-side change (§2.2).

### 8.3 How the pairing probe decides

"No text" and "no pairing" are the same zero, and screen length is not a test (aria-labels inflate a
player screen; the browse feed is long but is not settings). So a **negative requires ≥2
settings-screen markers** (`Linked devices`, `Restricted Mode`, `Privacy`, `Sign out`, …) on the
captured screen; the positive is never gated, because seeing the phrase is seeing it. Real captures
of the watch screen, the watch screen with aria-labels, and the browse feed are L0 fixtures in
`tests/harness/test_feature_probe.py`.

### 8.4 P12.4 — touch works, and the capability query lies

`navigator.maxTouchPoints == 0`, `'ontouchstart' in window == false`, `(pointer: coarse) == false` —
Chromium finds no Direct-mode XI2 touch device. **Touch is delivered anyway.** At mode 4, 9 taps
produced 9 `touchstart` + 9 `touchend` with per-finger ids and page-space coordinates; every gesture
has been driven by a finger (`durable/touch-gestures.md`). Multitouch too: 5 multi-finger sequences
in 48.

**This generalises past touch:** every natural feature-detection here returns the wrong answer on a
device where the feature works. Anything gated on them would disable itself exactly where it should
run, which is why the gesture layer is config-gated rather than detected.

## 9. Desk research (no device)

Sources: a sibling Leanback client's source and all 279 of its issues
(github.com/shy1132/VacuumTube, "VT#n" below), the SmartTube and DeckTube READMEs, the Decky store
API, SteamOS 3.8 coverage, and the fork research on branch `backup/jul08-refactor`. Tags: **[V]**
checked in a primary source; **[I]** inference; **[U]** unverified. Engine-source facts were checked
at the **m114** pin `a9181df`, not M138: re-verify after a sync. Tasks: TASKS P12.15–P12.22.

### 9.1 New candidates (not shipped, planned, or rejected before)

| Task | Feature | Evidence of demand | Mechanism / main risk |
|---|---|---|---|
| P12.15 | 60 fps unlock | VT `media-capabilities.js`; SmartTube lists 60 fps as a headline feature | page script rejecting absurd `framerate/width/eotf` in `isTypeSupported` / decode load, must stay on `VaapiVideoDecoder` |
| P12.16 | Long-press A → context menu | VT#57 (Enter held ~500 ms), VT#282 | launcher sends down/up separately / auto-repeat may cancel the timer |
| P12.17 | Resume after a crash | [I] | `t=` deep link / Leanback may ignore `t=` |
| P12.18 | `h5vcc` shim → deep links, then DIAL cast target | VT `h5vcc/index.js`; VT#229/#260/#269 | page script + launcher SSDP/HTTP / Leanback may assume Cobalt platform services once `h5vcc` exists |
| P12.19 | Pause on headphone unplug | [I] phone-standard | evdev `SW_HEADPHONE_INSERT` / jack may not expose a switch; BT needs a permission or the host helper |
| P12.20 | SABR/attestation canary + honest error | VT#189/#198/#224/#258 (Jun–Sep 2026) | harness + toast / a guest canary may never hit an enforced account |
| P12.21 | Channel/playlist Steam tiles | VT#80, VT#112 | deep-link grammar / route drift |
| P12.22 | Remaining time / "ends at" | VT#254 (merged) | page script / `idomkey` drift |

Unranked: dock state via gamescope's read-only `GAMESCOPE_DISPLAY_IS_EXTERNAL` atom — useful only as a
*trigger* (dock/AC drives P12.9; turning touch off when docked). Value low.

### 9.2 Facts worth keeping

- **Leanback maps console gamepad codes itself** [V VT `controller-support.js`, untrusted DOM
  events]. The codes are `0x8000 + button` (Starboard `kSbKeyGamepad*`), plus `MediaPlayPause`
  `0xB3` and `Subtitle` `0x1CC`; all three exist in `starboard/key.h` [V m114 tree]. Whether a CDP
  `windowsVirtualKeyCode` of 32768+ surfaces as `event.keyCode` is [U]. If it does, pending bindings
  can send native codes instead of hand-built behaviour (input-ux §8.1).
- **Chrobalt's `h5vcc` has no `dial` module** [V m114 tree], and our build exposes no `h5vcc` at all
  (`preview.md`).
- **A UA override without `userAgentMetadata` turns off the Sec-CH-UA client hints** [V m114 tree,
  `client_hints.cc`], so our requests do not present a second, desktop identity (the mismatch class
  behind VT#258). What `navigator.userAgentData` returns is [U].
- Leanback's max quality follows `h5vcc.system.getVideoContainerSizeOverride`; VT had to stop a
  1280×800 screen rounding down to 720p (VT#209) [V].
- The cast label is the feature switch `mdx_device_label` [V VT `cast-label.js`].
- AI "super resolution" formats are tagged in `adaptiveFormats[].xtags` [V VT source]. Filtering them
  is response tampering (A3); keep this as a bitrate/battery fact only.

### 9.3 Caveats on §2 and §8

- **The `hd720` ceiling has two possible causes**: 720p plausibly from the missing container-size
  override (§9.2), and 30 fps from Chromium ignoring Cobalt's extra MIME parameters. §8.1's labels
  (no `720p60`) are suggestive only — that video may have had no 60 fps rendition. P12.15 decides.
- **R1/R2 drift to watch:** attestation enforcement is reaching sibling clients, guests are asked to
  sign in "to prove you're not a bot" (VT#158/#233), and the late-2025 TV watch-screen redesign is DOM
  drift risk for the `idomkey`/overlay scripts. The smoke canary (guest, UI-only) catches none of
  these: P12.20.
- **SteamOS 3.8** adds preliminary LCD hibernation, wake from a Steam Controller, and "better scaling
  on TVs" [V secondary]. `PrepareForSleep` should cover hibernate [I].

### 9.4 Still not recommended

As §5/§6, plus: Discord RPC, offline downloads, PO-token/attestation spoofing, and a Chromecast
(`CrKey`) UA (lands in cast-receiver idle, m114 S0.2).
