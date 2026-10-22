---
scope: durable
topic: input-and-ux
verified: 2026-07-09
sources: research passes on Deck hardware, mobile/iOS touch, console/TV controllers, hybrid UX, Deck system keys + OSK, the Leanback/Cobalt key contract (tree-verified), 10-foot vendor guidelines; on-Deck spikes (OLED) — URLs inline
---

# Input & UX — cross-device best practices for the Deck Leanback client

Deck hardware facts and YouTube/TV UX conventions; survives Cobalt bumps. The architecture principle
holds throughout: **all platform-dependent input lives in `launcher/` (the platform-abstraction
layer, Starboard-analogue) and reaches the engine only via CDP — no engine patch.**

## 0. The one decision everything hangs on
**Lead with the controller-first 10-foot (Leanback) model; touch + trackpad are secondary, opt-in
pointer layers.** Game Mode is controller-first; Valve Verified requires the *default* controller
config to reach *all* functionality; we render `youtube.com/tv`, already a focus-driven spatial-nav
UI whose large tiles are trivially tappable, whereas retrofitting D-pad focus onto a touch-first UI
is costly and fragile.
[Valve compat](https://partner.steamgames.com/doc/steamdeck/compat) ·
[Android TV nav](https://developer.android.com/design/ui/tv/guides/foundations/navigation-on-tv)

## 1. What the app can actually see on the Deck (Game Mode)
Steam `EVIOCGRAB`s the physical controller and re-emits a **virtual gamepad** (`28DE:11FF`, evdev
name "Microsoft X-Box 360 pad"), which the launcher reads (confirmed on-device; see `hardware.md` for
the second wrapper node Steam adds). Consequences:
- **Standard controls** (ABXY, D-pad, L1/R1, L2/R2 analog `ABS_Z`/`ABS_RZ`, both sticks + L3/R3)
  arrive on the virtual pad → readable via raw evdev. ✅
- **Rear buttons L4/L5/R4/R5, trackpad touch/click, gyro** are **NOT** on the virtual pad — they reach
  an app **only via a Steam Input configuration** (`config/steam_input.vdf`), which today duplicates
  the grips onto face buttons (R4 → Y), so the launcher cannot tell a grip from its face button.
  [Alice's blog](https://nyaa.place/blog/steam-deck-hid-and-libmanette-adventures/)
- **STEAM, QAM (…), volume, power are permanently system-reserved** — never design a feature needing
  them. [SteamOS #1050](https://github.com/ValveSoftware/SteamOS/issues/1050)
- **Haptics:** whole-controller rumble only (dual-motor; no trigger rumble on Deck). The launcher
  uses evdev `EVIOCSFF`/`FF_RUMBLE` (`haptic.cpp`, §14). Trackpad haptics are Steam-Input-only.
- Node numbers are unstable across boots/resume — **enumerate devices by identity**, never hardcode
  `eventN`.

## 2. Canonical controller mapping (shipped `config/app.json:keymap`)

| Deck control | Action | Dispatched as | On-Deck |
|---|---|---|---|
| D-pad + left stick | `navigate` | Arrow keys (37–40), with auto-repeat | verified |
| **A** | `select` | `Enter` (13) | verified |
| **B** | `back` | `Escape` (27) | verified |
| **X** | `playpause` | `MediaPlayPause` (179) | — |
| **Y** | `toggle_gestures` | launcher action: touch gesture router on/off, toast + rumble; inert while `touch_gestures` is off | verified (with gestures on) |
| **LB / RB** | `scrub_back` / `scrub_fwd` | `ArrowLeft` / `ArrowRight` — Leanback's progress-bar scrub | — |
| **LT / RT** | `chapter_back` / `chapter_fwd` | `config/scripts/chapter_seek.js` over CDP; falls back to ±`skip_seconds` (§18) | verified |
| View | `toggle_captions` | `config/scripts/toggle_captions.js` over CDP (§8.1) | **unverified** |
| Menu (☰) | Settings | fixed launcher entry point, independent of the keymap | — |
| Right stick | fast scroll | arrow keys, analog rate (§15) | **unverified** |
| STEAM / QAM / vol / power | (OS-level) | not injected | — |

The keymap is hot-swappable: a value is either a semantic action or a DOM key dispatched verbatim,
and a DOM key wins. Contextual layers exist (`keymap_player`, `keymap_osk`, held-trigger modifiers
`keymap_lt`/`keymap_rt`) and ship empty — nothing goes in them without an on-Deck spike. Unmapped
actions (`player_menu`, `scan_rewind`, `scan_forward`) are logged at startup, never bound to a guess.

**Web Leanback key contract** (`youtube.com/tv`): Google publishes none. Only `ArrowUp/Down/Left/Right`,
`Enter`, `Escape` (27) are verified on-Deck (S0.6). Chrobalt is Chromium, so `keyCode` is the
**Windows virtual-key** set (§8.1); CEA-2014/HbbTV codes (`461` GoBack, `415` Play, …) are not native
here, and desktop watch-page shortcuts (`j`/`l`/`k`/`c`/`/`/`Shift+N/P`,
[answer/7631406](https://support.google.com/youtube/answer/7631406)) may not be assumed — `c` for
captions is confirmed ignored by Leanback.
[YT Help: consoles](https://support.google.com/youtube/answer/3013387) ·
[Suitest TV keys (the *other* keycode world)](https://suite.st/docs/faq/tv-specific-keys-in-browser/)

## 3. Touchscreen
- Device: Focaltech **`FTS3528` / `2808:1015`** (measured on the OLED), genuine multitouch (MT-B),
  `1280×800`. Its `eventN` **changes per boot** — enumerate by name/VID:PID/`ABS_MT_SLOT`.
  [crocidb](https://crocidb.com/post/investigating-touchscreen-issue-steam-deck/)
- gamescope's global `STEAM_TOUCH_CLICK_MODE` decides what the app gets. In the default mode a tap
  arrives as a synthetic mouse click at that point. At **mode 4 (passthrough)** real `wl_touch`
  reaches Blink, and **multitouch is delivered** (5 multi-finger sequences in 48); how many points
  arrive is unknown and gates any pinch work (`touch-gestures.md` §4). Valve's FAQ advice "don't rely
  on multitouch" was never a measurement.
- `navigator.maxTouchPoints` reads 0 on a panel where touch works, so touch cannot be feature-detected
  — it is config-gated.
- Shipped touch states (`touch-lock.md`, `touch-gestures.md`):
  - **`disable_touch` (default on):** `config/no_pointer.js` swallows every pointer/mouse/touch event
    in the page, and `touchmode.cpp` holds the touch mode at hover (0) while our window is focused.
  - **`touch_gestures` (default off, mutually exclusive; `disable_touch` wins):** mode 4 and a
    page-layer router using YouTube-mobile's spatial model — swipe = move focus, tap = Enter,
    double-tap left/right third = seek ∓`skip_seconds`, hold = 0.5× (left) / 2× (right), left-edge
    swipe = Back. [YT mobile gestures](https://www.howtogeek.com/749519/5-youtube-gestures-you-should-be-using-on-android-and-iphone/)
  - Both off: raw default mode, taps click.
- Vertical brightness/volume swipes are **not** stock YouTube and are OS-level on Deck — skip.

## 4. ★ Runtime touch block / unblock (the explicit requirement)
A grab-based lock does not work on SteamOS: the seat user cannot open the panel node, and
`EVIOCGRAB` does not starve gamescope of touch (`touch-lock.md`). The lock code is deleted. Touch is
made inert by `disable_touch` (§3), which needs no engage/disengage — it is simply on.

**Policy** (applies to any touch state the user can change):
- Never a silent, total, mode-gated disable (the Switch accessibility anti-pattern). Changing touch
  state from the controller is visible: the Y toggle toasts the new state and rumbles, because the
  moment you want touch off is usually the moment a palm is on the panel.
- **The controller is never disabled** (Verified: the default config must reach everything).
- Never mix touch (absolute) and trackpad (relative) on one shared cursor → the Deck
  "previous-location click" bug.
  [Deck touch bug](https://steamcommunity.com/app/1675200/discussions/1/4338725580140497497/)

## 5. Design constraints (Valve Deck-Verified + 10-foot, our compliance target)
- **Default controller config reaches ALL functionality**; users must not tweak settings to get
  controller support. Ship a good `steam_input.vdf` + grid art.
- **Glyphs match the active device** (Deck/Xbox); no mouse/keyboard prompts while on controller.
  Track `currentModality` for cosmetics only — **never gate whether an input functions** (Valve's
  last-wins warning; accumulate, don't lock). [recommendations](https://partner.steamgames.com/doc/steamdeck/recommendations)
- **Text legibility:** char height ≥ **9px @1280×800** (aim 12px; TV scale prefers ~24px body).
- **Targets 44–48px**, always-visible focus ring, one focused element, cheap single-step Back with no
  confirm dialog, comfortable D-pad auto-repeat (fast but not overshooting).
- **Text entry** = Leanback's own on-screen keyboard driven by D-pad (Arrows+Enter work) — do not
  force per-character-only typing.

## 6. Implications for our code
- The mapping lives in `config/app.json` (hot-swappable), not C++ (§2).
- **B sends `Escape` only.** `Backspace` *is* `Back` in Cobalt (`kSbKeyBack == kSbKeyBackspace == 8`),
  so an Escape+Backspace fallback chain is a footgun inside the OSK, and `461` is a foreign (HbbTV)
  keycode.
- Rear buttons/trackpad/gyro + glyphs are a **Steam Input config** deliverable, not evdev.
- Rumble is evdev force-feedback (`haptic.cpp`), not SDL2 — holds the dependency budget.

## 7. Gap analysis vs mobile YouTube / tablets / hybrid handhelds
Benchmarks: current mobile-YouTube gestures, the Switch YouTube app (closest hybrid analog; it
shipped with no swipe support at all —
[Nintendo Support](https://en-americas-support.nintendo.com/app/answers/detail/a_id/42563)), and the
PS5 app (parity benchmark). Everything stays in the launcher/config layer.

| Gap | State |
|---|---|
| Touch scrolling and a touch Back path | **shipped** in the gesture layer (swipe, left-edge Back), default off (§3) |
| Hold-for-2× | **shipped** as the touch hold; a controller speed binding is open (TASKS P12.5) |
| Chapter-aware seeking | **shipped** on LT/RT (§18) |
| Two seek models | three are bindable: scrub (LB/RB), chapter (LT/RT), fixed skip (`skip_back`/`skip_fwd`); SUPPORT.md must describe what ships |
| Context-free keymap | layers exist (`keymap_player`/`keymap_osk`/`keymap_lt`/`keymap_rt`), ship empty pending spikes |
| OSK context | open: B = Escape inside the OSK likely exits search rather than deleting (§8.1, §8.4 #1). Switch's in-keyboard bar is Y=space, B=delete ([Miketendo64](https://miketendo64.com/2018/11/09/guide-what-can-i-do-with-youtube-on-nintendo-switch-and-what-are-the-controls/)) |
| Right stick unused | **shipped** fast traversal (§15) |
| First-run controls card | **shipped** (§17) |
| Kiosk error/retry page | **shipped** (§16) — also the R1 hotfix surface |
| Remap story | Steam Input's physical→xinput rebinding already gives per-user remap without touching `app.json` |
| "Latency ≤ PS5" | open — log evdev-receipt→CDP-ack per key; calibrate once against a 120/240 fps camera |
| Pinch/zoom-to-fill for the 16:10 panel | post-1.0 stretch (doc §10) |

---

# Part II — Text entry, the Deck's keyboard keys, and 10-foot conventions

## 8. Text entry & keyboard keys

### 8.1 ★ Two incompatible keycode worlds — do not mix them
Chrobalt is **Chromium**, so `KeyboardEvent.keyCode` carries **Windows virtual-key (VK)** values.
Cobalt's `SbKey` enum *is* the VK set, so the SbKey→DOM mapping is an identity for these codes. The
CEA-2014/HbbTV codes TV-development docs enumerate belong to **Tizen / webOS / HbbTV browser**
builds and are **not** our engine's codes.

**Tree-verified** against the pinned checkout, `cobalt/src/starboard/key.h`:

| Semantic | Ours (Chromium/Cobalt VK) | The *other* world (CEA/HbbTV) |
|---|---|---|
| Back | `kSbKeyBack` **= `kSbKeyBackspace` = 0x08 (8)**; `kSbKeyBrowserBack` = 0xA6 (166) | 461 / Tizen 10009 |
| Escape | 0x1B (27) | — |
| Delete (forward) | `kSbKeyDelete` = 0x2E (46) | — |
| Play/Pause · Stop | 0xB3 (179) · 0xB2 (178) | 415 / 19 · 413 |
| **Rewind · Fast-forward** | **`kSbKeyMediaRewind` = 0xE3 (227) · `kSbKeyMediaFastForward` = 0xE4 (228)** | 412 · 417 |
| Prev · Next track | 0xB1 (177) · 0xB0 (176) | — |
| Microphone | `kSbKeyMicrophone` = 0x3002 (12290) | — |

`key.h:36` reads `kSbKeyBack = kSbKeyBackspace,` with a comment steering callers to `kSbKeyEscape`
for semantic back. Consequences:

- **`Backspace` IS `Back` at the platform layer**, so "B deletes a character inside the OSK" may pop
  the search view instead.
- **`MediaRewind` (227) / `MediaFastForward` (228)** are in `devtools.cpp:kKeys`, so they can be
  dispatched as raw key values; Leanback's response is unverified, so `scan_rewind`/`scan_forward`
  stay unmapped.
- **Captions are not a key.** `c` is the desktop caption hotkey; Leanback receives it and does
  nothing. `toggle_captions` is a launcher action that runs `config/scripts/toggle_captions.js` over
  CDP, toggling the HTML5 player's caption module (`getOption`/`setOption` `tracklist` + `track`;
  module name `captions` on HTML5, `cc` on legacy — the script probes both; hot-swappable).
  On-Deck functional verification is pending.

The mapping is *enum*-verified, not *behaviour*-verified: what Leanback's obfuscated JS listens for
is unknowable statically. Our CDP path sets `windowsVirtualKeyCode` explicitly, so we *can* send 461 —
it is simply not native, and no evidence says the app honours it.
[youtube/cobalt `starboard/key.h`](https://raw.githubusercontent.com/youtube/cobalt/main/starboard/key.h)
· tree: `cobalt/src/starboard/key.h`, `cobalt/src/ui/events/keycodes/keyboard_code_conversion_starboard.cc`

### 8.2 ★ Voice search is not a key — it is a platform service we do not have
`kSbKeyMicrophone` (12290) exists, but Cobalt routes voice search through the
**`SoftMicPlatformService` Starboard extension** (`MediaRecorder` + `SbMicrophone`), not a DOM key,
and this Chromium-based binary has no Starboard platform service at all (S0.5: zero starboard
symbols). Injecting 12290 will do nothing. The only viable path is Leanback's own on-screen
**soft-mic button** backed by a real `getUserMedia` mic (§13). Voice is **not shipped**: the
launcher's implementation was removed because the mic never worked on hardware; the mic auto-grant
(`mic_autogrant`) remains. [Cobalt voice_search doc](https://developers.google.com/youtube/cobalt/docs/gen/cobalt/doc/voice_search)

### 8.3 ★ The Deck's own keyboard keys, and the OSK trap
Only **three** system combos emit real keyboard keys to a focused app; everything else on the STEAM
chord is intercepted. Valve publishes this only as an on-device overlay, so the table is corroborated
across 5+ independent sources rather than first-party.

| Combo | Effect | Emits a key? |
|---|---|---|
| **STEAM + D-pad Right / Down / Left** | **Enter / Tab / Escape** | **yes — real keyboard keys** |
| STEAM + X | Summon OSK | opens OSK (see trap below) |
| STEAM + B (hold) · STEAM + R1 · STEAM + L1 (hold) | force-quit · screenshot · magnifier | no |
| STEAM + L2 / R2 · STEAM + right stick/pad | right/left mouse click · cursor | mouse only |
| STEAM + left stick U/D | brightness | no |
| **STEAM · QAM (…)** | always Steam menus | **never reach the app** |

- **⚠ The OSK trap.** For non-Steam, non-"game-like" apps (Chrome and Discord are the cited
  examples), **STEAM+X in Game Mode can summon the _desktop_ OSK and soft-lock the session.** The
  reliable path is **QAM ("…") → keyboard button**. Deckback is exactly this shape; SUPPORT.md says
  so. [steam-for-linux#9117](https://github.com/ValveSoftware/steam-for-linux/issues/9117) ·
  [SteamOS#855](https://github.com/ValveSoftware/SteamOS/issues/855)
- **No auto-OSK on `<input>` focus under Xwayland.** gamescope raises the OSK for native Wayland
  clients via **text-input-v3**; there is no standard way for X11 clients, which is also why
  `SDL_StartTextInput` is a no-op on Deck. We run `--ozone-platform=x11` (S0.4), so **the keyboard
  never pops up by itself.** The clean fix — becoming a native Wayland client — is an engine/ozone
  change that collides with S0.4. [gamescope#668](https://github.com/ValveSoftware/gamescope/issues/668) ·
  [crbug 40113488](https://issues.chromium.org/issues/40113488)
- The Steamworks OSK APIs (`ShowFloatingGamepadTextInput`, which synthesizes OS key events and would
  work over Xwayland) require linking Steamworks. **Deckback does not and will not.** Valve's
  "auto-invoke an OSK whenever text input is required" is **structurally unmeetable for us** — state
  it openly. [ISteamUtils](https://partner.steamgames.com/doc/api/ISteamUtils)
- **Physical USB/BT keyboards do reach the focused Xwayland app** in Game Mode, but Game Mode
  **forces QWERTY scancodes**, ignoring the configured layout (the OSK respects it).
  [SteamOS#798](https://github.com/ValveSoftware/SteamOS/issues/798)
- **Steam Input can emit arbitrary keyboard keys**, including for non-Steam shortcuts, resolved by
  **scancode against the system layout**. Our `config/steam_input.vdf` is xinput passthrough and uses
  none of this — a live fallback if CDP key injection ever fails, and the per-user remap layer (§7).
  [Steam Input keyboard binds](https://steamcommunity.com/sharedfiles/filedetails/?id=3172761874)

### 8.4 What only an on-Deck spike can answer (bind nothing before these pass)
Every item is *enum*-verified but *behaviour*-unverified. Instrument first — install a capturing
listener, then inject one code at a time:

```js
window.__log = [];
['keydown','keypress','beforeinput','input','textInput','keyup'].forEach(t =>
  addEventListener(t, e => __log.push({t, key:e.key, keyCode:e.keyCode,
    tgt:e.target.tagName, val:e.target.value, prevented:e.defaultPrevented}), true));
```

1. **OSK erase semantics** — with the search field focused, inject `Backspace`(8), `Escape`(27),
   `Delete`(46) separately; after each, read the field value *and* whether the view popped (§8.1
   predicts 8 navigates).
2. **Direct printable typing** — inject `'a'` (65, with `text:"a"`). Does `.value` gain "a"? Do
   `keypress`/`beforeinput`/`textInput` fire? Decides whether a real/BT keyboard can type into
   Leanback, or whether its grid OSK is D-pad-only. **Highest-value spike of the set.**
3. **Transport keys** — during playback inject 179/178/227/228/176/177 and colors 403–406, with CEA
   415/461 as negative controls. Decides `scan_rewind`/`scan_forward` and prev/next. (Seeking itself
   is solved through the player API, §18.)
4. **Soft-mic reachability** — §13.5.
5. **Speed** — no TV-app hotkey is documented; the player API exposes rate control
   (`feature-landscape.md` §8.1).

## 9. 10-foot guidelines on a 7" panel — the angular-equivalence rule

Visual *angle*, not pixels, is what the eye resolves. Screen-height / viewing-distance:
- 55" TV at 3 m → 686 mm / 3000 mm ≈ **0.229 rad**
- Steam Deck at 40 cm → 94 mm / 400 mm ≈ **0.235 rad**

Within ~3%. **Any 10-foot guideline expressed as a fraction of screen height transfers to the Deck
≈1:1.** Guidelines in *absolute pixels on a 1080p canvas* do **not**, because we render 720p and
letterbox. The exceptions:

| TV guideline | On the Deck |
|---|---|
| TV-safe / overscan margins (~5%; Android TV 58dp/28dp) | **INVERTED — drop.** Overscan is a TV-hardware artifact; the Deck panel has none. (Leanback applies its own server-side; we cannot remove those.) |
| Elevated contrast beyond WCAG 4.5:1 | **Keep** — the reason changes from *distance* to *glare* on a glossy handheld panel. |
| "TV-safe colors" / avoid banding-prone saturation | **Mostly drop** — OLED near-black smearing is the only residue. |
| Large typography (Fire TV ~28px @1080p floor) | **Transfers by angle** (~21px on our 800-tall buffer). The hazard is Leanback's *smallest* metadata text (~14–16px @1080p → ~11–12px here), near Valve's floor and rendered from a 720p buffer. **Measure the smallest rendered glyph on-device.** |
| Strong multi-cue focus ring (scale + border + glow) | **Transfers** — glare degrades a color-only ring even at 40 cm. |

**Valve's hard numbers** (the Deck-Verified gate): smallest on-screen character **≥ 9 px at
1280×800**, **12 px recommended**; default controller config reaches **all** functionality with no
in-app settings changes; no keyboard/mouse glyphs under the default config.
[Deck compat](https://partner.steamgames.com/doc/steamdeck/compat) ·
[recommendations](https://partner.steamgames.com/doc/steamdeck/recommendations)

**Two Verified requirements we structurally cannot meet, and must document rather than fake:**
1. **Glyph correctness** — Leanback renders its own generic/remote glyphs server-side.
2. **Auto-invoked OSK on text input** — requires Steamworks or Wayland text-input-v3 (§8.3).

**Adopted checklist:**
- One focused element, always visible, multi-cue ring. *(Leanback's job; don't break it.)*
  [Android TV focus](https://developer.android.com/design/ui/tv/guides/styles/focus-system)
- Every direction from every element leads somewhere — no focus traps. Test every direction on every
  surface. [Android TV nav](https://developer.android.com/design/ui/tv/guides/foundations/navigation-on-tv) ·
  [Xbox](https://learn.microsoft.com/en-us/windows/uwp/xbox-apps/tailoring-for-xbox)
- Back is one predictable step, never behind a confirm dialog. Repeated Back must never land on a
  blank shell or Chromium chrome. **Back never quits**; exit is the OSD's Y-hold (TASKS P3).
- **D-pad auto-repeat is ours to own**: ~300–500 ms initial delay, ~4–8 repeats/s. Windows'
  1000 ms/31 Hz default is wrong for list scrolling.
- Text entry: voice/dictation as a first-class path, not per-character D-pad typing; preserve
  Leanback's phone-pairing flow; QWERTY over alphabetical.
  [tvOS HIG](https://developer.apple.com/design/human-interface-guidelines/focus-and-selection) ·
  [Android TV OSK](https://developer.android.com/training/tv/get-started/onscreen-keyboard)
- Errors: an in-app, **controller-focusable Retry** — never a Chromium `net::ERR_*` interstitial (§16).
  [Roku principles](https://developer.roku.com/dev/docs/key-design-principles)
- First-run: one-shot, controller-dismissable, re-openable controls overlay (§17).
  [Apple onboarding](https://developer.apple.com/app-store/onboarding-for-games/)

## 10. Current code and docs against Part II
- `devtools.cpp:kKeys` has `MediaRewind` (227) and `MediaFastForward` (228); binding them waits on
  §8.4 #3.
- B sends `Escape` only (§6).
- Voice is not a keymap problem (§8.2); Y is `toggle_gestures`.
- SUPPORT.md tells users to open the OSK from the **QAM menu, not STEAM+X**, and that a physical
  keyboard is forced to QWERTY.
- Deck-Verified glyph correctness + auto-OSK are **structurally unmeetable**; say so rather than
  claim Verified parity.
- Auto-repeat (`input.cpp`): 350 ms initial delay, 130 ms base interval (~7.7 Hz), accelerating to a
  55 ms floor — inside the recommended band. The acceleration has never been exercised on-device.
- Nothing here implies an engine patch.

## 11. Touch is all-or-nothing — the hidden price of a gesture layer
A gesture layer must **own** every touch it recognises, or the platform's default handling
double-acts (our double-tap seek *and* a synthetic click both fire). The planned way to own touch —
an exclusive `EVIOCGRAB` on the panel, which would have made tap-to-activate, palm rejection and
crash-safety our problem — does not exist on SteamOS (`touch-lock.md`). The shipped router owns
touch in the page instead: gamescope mode 4 delivers real touch events, and
`config/scripts/touch_gestures.js` recognises gestures in the capture phase and swallows them, so
Leanback cannot double-act. Tap-to-activate is the router's job (tap → `Enter`), delivered as a
trusted key by the launcher (`touch-gestures.md`).

## 12. The seek model, and the triggers
- Arrow keys in the Leanback player are the console **scrub** model, not an instant ±10 s jump. LB/RB
  ship as `scrub_back`/`scrub_fwd`; the old names `seek_back_10`/`seek_fwd_10` still resolve to scrub
  for one release and warn at startup.
- The instant ±10 s desktop keys (`j`/`l`) are not a TV contract (§8.1). Fixed-interval and chapter
  seeks go through the player API (§18), not keys.
- **L2/R2 arrive as analog `ABS_Z`/`ABS_RZ`** under the shipped `steam_input.vdf` (verified on-Deck),
  so `trigger_pressed()`'s press/release hysteresis fires. `lt_down_`/`rt_down_` track held state: a
  non-empty `keymap_lt`/`keymap_rt` turns that trigger into a held modifier that stops dispatching
  its own action and absorbs controls it does not bind (no LT+A firing a plain A). Both layers ship
  empty.

## 13. Voice input — hold-to-talk, and why it cannot be a key
**Not shipped.** The launcher's voice implementation was removed because the mic never worked on
hardware. What carries over to a reimplementation:
the Deck has a dual mic array; the launcher still auto-grants `audioCapture` to the app origin
(`mic_autogrant`, `Browser.grantPermissions`); `devtools.cpp` has
`mouse_press`/`mouse_release`/`mouse_click` (`Input.dispatchMouseEvent`). The product ask is the TV
convention: **hold a button, speak, release.**

### 13.1 The mechanism is a click, not a keypress
There is no voice key at any keycode (§8.2). The only path is to **activate Leanback's own on-screen
soft-mic button**:

| | How | Verdict |
|---|---|---|
| (a) focus-nav | D-pad focus onto the mic button, then `Enter` | Fragile — depends on where focus is, and on the search view being open |
| (b) **trusted synthetic click** | `Runtime.evaluate` → `getBoundingClientRect()`; `Input.dispatchMouseEvent` at its centre | **Preferred.** Position-independent; CDP mouse events are trusted |

**CDP mouse coordinates are CSS pixels of the viewport**, so `getBoundingClientRect()` feeds
`dispatchMouseEvent` directly — no letterbox transform.

### 13.2 ★ The underrated risk: our UA may hide the mic button
We ship a **Cobalt** UA. Leanback may assume the platform provides voice via
`h5vcc`/`SoftMicPlatformService` and either not render a web soft-mic button, or render one whose
handler calls a platform API that does not exist here. **Our own identity bet (R1) could be what
breaks voice.** Check first, before binding anything:

```js
({ h5vcc: typeof window.h5vcc,
   mic: !!document.querySelector('[aria-label*="voice" i],[aria-label*="search by voice" i],[class*="mic" i]') })
```

If Leanback gates voice on `h5vcc`, a **patch-free hypothesis**: shim the platform service in an
injected script via `Page.addScriptToEvaluateOnNewDocument` (the technique AV1 steering uses),
advertising a soft mic with `micGesture: "HOLD"`. The service's JSON protocol is Cobalt-internal and
undocumented — an **unverified hypothesis, not a plan**. If it fails, voice is not deliverable on this
engine; say so rather than ship a dead button. This probe (V0) has never run.

### 13.3 Speaker bleed — the handheld-specific problem
Hold-to-talk while a video plays feeds the Deck's speakers into its own mics ~15 cm away. **Pause or
duck playback for the duration of capture, before the mic opens, and restore on release** (only if
we changed it). This likely determines whether server-side ASR returns anything usable; a voice
spike tested in silence has not tested the real case.

### 13.4 Which button
Y is now `toggle_gestures`, so voice needs a new home. Rear grips are not on the virtual pad (§1);
the only route is a Steam Input binding mapping a grip to a *distinct* standard xinput button — today
all four grips duplicate face buttons (TASKS P12.7). Mapped buttons dispatch on the press edge;
hold-to-talk needs press → start, release → stop, with a short debounce so a stray tap does not open
the mic (the OSD's Y-hold exit is the existing release-edge precedent in `input.cpp`).

### 13.5 Spike ladder — stop at the first failure
1. **`h5vcc` present? mic button in the DOM under our UA?** (§13.2). If no button and no shim, stop.
2. Is the button focusable/activatable at all (D-pad + `Enter`)?
3. Does a **trusted `Input.dispatchMouseEvent`** on its rect start capture — `getUserMedia` called,
   **no permission prompt** (the auto-grant is unverified on-Deck)?
4. Does the page honour **hold** (`micGesture: "HOLD"`), or is it tap-to-toggle? If toggle, emulate
   hold: click on press, click again on release.
5. Is the captured track **live and non-silent** through the Flatpak's pulse socket? Assert on audio
   levels, not on the absence of an error.
6. **Speaker bleed:** does ASR still resolve with a video playing? Compare ducked vs not (§13.3).

Nothing gets bound until 1–3 pass. A dead button is worse than a missing one.

## 14. Touch-lock feedback — making the lock observable, and the asymmetry of lock vs unlock
The grab-based lock is deleted (§4), so nothing here gates touch. The feedback primitives it produced
are shared by the gesture toggle, the captions toast and the controls card:

- **Toast** (`config/scripts/toast.js`): one reused `<div id="__deckback_toast">` appended to
  `document.documentElement` (not `body` — Leanback replaces body content on navigation),
  `pointer-events:none` so it never swallows input. Markup goes through a **Trusted Types policy**:
  `youtube.com/tv` enforces Trusted Types, and a bare `innerHTML` assignment is a **silent no-op**
  there (`../milestones/m138.md` §"youtube.com/tv page constraints"). Renders over real Leanback
  (verified).
- **Haptic** (`haptic.cpp`): evdev `EVIOCSFF` + `FF_RUMBLE`, on a separate `O_RDWR` fd (the input
  layer's are read-only) opened lazily on the first pulse. A pad without `EV_FF` is ordinary: log
  once, toast only. TEST-PLAN §2 records the gesture toggle's toast + rumble as verified on the OLED.
- **Lock vs unlock are not symmetric.** For any future user-facing lock: locking can be immediate,
  unlocking needs a deliberate hold — an accidental unlock hands the panel straight back to the palm
  already on it. Use different rumble magnitudes for the two directions so a user who is not looking
  can tell which way it went.
- **A silent state change is a bug report about something else**: a disabled input and a hung app
  look identical from the user's seat.

## 15. Right-stick fast traversal — why its ramp must NOT look like the D-pad's
`fast_scroll()` (`input.cpp`, pure + unit-tested): the right stick emits the same arrow keys as the
D-pad, through the same layer/modifier machinery, so it adds **no new key assumption**.

| | Signal | Correct ramp |
|---|---|---|
| D-pad / left stick | digital: "held" or not | accelerate over **time** — nothing else is available |
| Right stick | analog deflection | rate **is** deflection — the thumb is the accelerator |

A time ramp on the right stick would put two controllers on one variable. So: `right_stick_slow_ms`
(200) at the deadzone edge, linear down to `right_stick_fast_ms` (45) at full deflection, re-read from
the *current* deflection on every step. Constraints, each a unit test:

- **`fast_ms` is a floor, not a target.** Each arrow is a `keyDown` + `keyUp`, both awaited over CDP;
  below ~25 ms the input thread lives in the socket and stops draining evdev. A hot-swapped
  `app.json` is untrusted, so the floor is enforced in code.
- **The axis minimum is `-32768`, one past the `+32767` maximum.** Unsaturated, the down-left corner
  resolves **Left** while down-right resolves **Down**. Saturate before comparing.
- **The dominant axis wins, vertical takes the tie** — unlike the digital `resolve_direction()`,
  where vertical always wins.
- Suppressed while the D-pad or left stick hold a direction: one intent never emits two arrow streams.
- Normalised against the Xbox-pad range, not per-device `absinfo` (the merged multi-device path has
  no single range). A narrower-range pad scrolls slower, never faster.

**Unverified on hardware:** that the virtual pad emits `ABS_RX`/`ABS_RY` under our
`steam_input.vdf`, and that Leanback's rails keep up with a 45 ms cadence. Both fail safely (no
scroll, or a slower scroll).

## 16. The kiosk failure state, and how a failed navigation is detected
**Detection rule:** `Page.navigate` reports a *failed* navigation as a **successful CDP command**
carrying an `errorText` field, and the failed document keeps `document.location.href` set to the
requested URL — so "href contains `www.youtube.com/tv`" cannot tell a loaded TV app from Chromium's
error interstitial (whose only control is a mouse-clickable Reload). `DevToolsClient::navigate_checked()`
returns `{sent, error_text}`: **`sent == false` is transport** (the engine is gone — retry the socket),
**`error_text != ""` is navigation** (the engine is fine, the network is not — draw a page).

**The page** (`errorpage.{hpp,cpp}`, `config/scripts/error_page.js`): `about:blank` + a CDP-injected
document. Not a bundled `file://` page (one more sandbox path, and it can fail with the error it
reports) and not a `data:` URL (restricted as a top-frame navigation); `about:blank` always commits.

- A **focused** "Try again" button with a visible focus ring — on a controller an unfocused button is
  unreachable. Enter and Space retry.
- **Escape is deliberately unbound** — the only thing it could do is quit.
- Automatic retry with exponential backoff (`error_retry_min_ms` 2 s → `error_retry_max_ms` 30 s). A
  user-requested retry **resets** the backoff.
- The retry signal runs page → launcher (`window.__deckbackRetry`, read-and-cleared in one round
  trip); the launcher owns *when* to retry, because only it knows whether the navigation succeeded.
- `classify_net_error()` maps the `net::` code to actionable advice and **refuses to guess** on codes
  it does not recognise.
- **This is the R1 hotfix surface.** `error_title`/`error_hint` ship in `app.json`, escaped on the way
  into the page. `error_page: false` restores Chromium's page — and the launcher then stops noticing
  the failure.

**Unverified on hardware:** that the injected document renders on `about:blank` under gamescope, and
that injected Enter reaches its Retry button. L2 test: pull the network, assert the Retry button has
focus.

## 17. The first-run controls card, and why it must not be written in C++
View = captions is unguessable, and `docs/SUPPORT.md` is invisible from Game Mode, so the app shows a
one-shot controls card (`onboarding.cpp`, `config/scripts/overlay.js`). After dismissal the same live
list is in Settings ▸ Keys, opened with the fixed Menu button.

**The card is generated from `app.json`, never hardcoded** — `app.json` is hot-swapped to answer
Leanback breakage (R1), and a card that keeps advertising an old binding teaches users to stop
reading it. Rules, unit-tested against the shipped `config/app.json`:

- **A control that resolves to no DOM key is omitted** — unless it is a launcher action
  (skip/chapter seek, captions, gesture toggle), which is listed explicitly.
- **Touch gesture rows appear only when the router is running** (`touch_gestures` on).
- Menu, D-pad/left stick and (when enabled) right stick are described independently of the keymap.
  `start` and the legacy value `show_controls` are skipped as keymap rows; Menu always opens Settings.

**The card is modal, enforced by the launcher**: while it is up, the input layer swallows every
controller event, so nothing moves behind it. **Only a button dismisses, never a stick or D-pad** — a
stick drifts at rest.

The marker is written **on show, not on dismiss** (a crash during onboarding must not re-show it
forever) and is versioned (`first_run_v1`), so a materially changed keymap can show the card once
more. It lives in `$XDG_STATE_HOME/deckback/`, not in the Chromium profile.

**Verified:** the card renders over real Leanback (Trusted Types policy, §14). **Unverified on
hardware:** that swallowing events at the evdev layer keeps focus still behind the card. L2 test:
show the card, press D-pad, assert `document.activeElement` did not move.

## 18. ★ Seek spike (§8.4 #3 / §12): `player.seekBy` works, chapter data comes from `/next` (on-Deck, 2026-07-13)

**The player object is `.html5-video-player`** (id `ytlr-player__player-container-player`), not
`#movie_player` — that id belongs to the desktop watch page and does not exist on `youtube.com/tv`.
It exposes `seekTo`, `seekBy`, `getCurrentTime`, `getDuration`, `getVideoData`, `getPlayerState`,
`getPlayerResponse`. Player scripts select `.html5-video-player || #movie_player || video`. **Any
player interaction must use this selector.**

**Fixed-interval skip** — `seekBy(±skip_seconds, true)` is a verified, TV-native jump.
`config/scripts/skip.js` (ScriptLibrary, hot-swappable); `skip_action_sign()` in `keymap.cpp`
recognises `skip_back`/`skip_fwd`; `input.cpp` prebuilds the script per trigger and `eval_void`s it
on the press edge, ahead of any DOM-key binding. `skip_seconds` defaults to 10. Verified on-Deck:
L2 = −10 s, R2 = +10 s.

**Chapter seek (the shipped LT/RT default)** — `config/scripts/chapter_seek.js` (params `dir`/`skip`);
`chapter_action_sign()` recognises `chapter_back`/`chapter_fwd`. Verified on-Deck.
1. Chapters come from `POST /youtubei/v1/next` with the page's TVHTML5 context +
   `INNERTUBE_API_KEY`: the response carries a `macroMarkersListEntity` in
   `frameworkUpdates.entityBatchUpdate.mutations[].payload`, whose `markersList`
   (`markerType: MARKER_TYPE_CHAPTERS`) has `markers[]` with `title.simpleText`, **`startMillis`**
   (string ms), `durationMillis`. Exact keys are milestone-scoped (`m138.md`).
2. "Prev" jumps to the current chapter's start unless within ~2 s of it, then the previous — the
   console convention. `seekTo(target, true)`.
3. `eval_void` does not await promises, so the script caches chapters on `window.__dbChapters` and
   fixed-skips on a cache miss while a background `/next` fetch warms it: the first press after a new
   video acts as a skip. No chapters / failed fetch → fixed skip, so LT/RT always do something.

**Dead ends (do not retry):**

| Source probed | Result |
|---|---|
| `getPlayerResponse().playerOverlays…multiMarkersPlayerBarRenderer` (desktop chapter path) | absent — the TVHTML5 player response has no `playerOverlays` |
| recursive walk (depth 9) of the player response for `/chapter\|marker/i` | zero hits |
| `videoDetails.shortDescription` (description-timestamp fallback) | empty — TVHTML5 strips it |
| `POST /youtubei/v1/player` forced to a **WEB** client context | `playabilityStatus: UNPLAYABLE` for every id, including guaranteed-playable ones — the request context is refused (fresh client-versions + `visitorData`/proof-of-origin tokens); chasing it is a yt-dlp-style arms race against our R1 posture |

**Off-watch guard.** Leanback keeps `.html5-video-player` in the DOM (paused, backgrounded) after the
user backs out to Home, and calling `seekTo`/`seekBy` there *resumes* it audibly. `skip.js`,
`chapter_seek.js` and the resume nudge `player_play.js` bail unless
`location.hash.indexOf('/watch') >= 0` — the same substring check `player_state.js` uses for
`Layer::Player`, kept identical so the two cannot drift. The guard is in JS, not `input.cpp`, because
it must read the route at the instant of the press; `layer()` is a poll result up to `poll_ms` stale.
Verified on-Deck for L2/R2 (2026-07-14); the `player_play.js` resume path shares the check but was not
exercised separately.
