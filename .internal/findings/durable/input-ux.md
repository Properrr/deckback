---
scope: durable
topic: input-and-ux
verified: 2026-07-09
sources: 5 research passes (Deck hardware, mobile/iOS touch, console/TV controller, hybrid UX; 2026-07-09 gap review vs mobile YT / Switch / PS5 (§7); 2026-07-09 pass 5 — Deck system keys + OSK, Leanback/Cobalt key contract (tree-verified), 10-foot vendor guidelines (§8–§10)) — URLs inline
---

# Input & UX — cross-device best practices for the Deck Leanback client

Synthesized from four research passes. Durable (Deck hardware facts + YouTube/TV UX conventions;
survives Cobalt bumps). Drives Phase 3 (input) and the touch block/unblock requirement. The
architecture principle holds throughout: **all platform-dependent input lives in `launcher/` (the
platform-abstraction layer, Starboard-analogue) and reaches the engine only via CDP — no engine patch.**

## 0. The one decision everything hangs on
**Lead with the controller-first 10-foot (Leanback) model; touch + trackpad are secondary, opt-in
pointer layers.** Rationale: Game Mode is controller-first; Valve Verified requires the *default*
controller config to reach *all* functionality; we literally render `youtube.com/tv` (already a
focus-driven spatial-nav UI); its large focusable tiles are trivially tappable, so touch comes "for
free," whereas retrofitting D-pad focus onto a touch-first UI is costly and fragile.
[Valve compat](https://partner.steamgames.com/doc/steamdeck/compat) ·
[Android TV nav](https://developer.android.com/design/ui/tv/guides/foundations/navigation-on-tv)

## 1. What the app can actually see on the Deck (Game Mode)
Steam `EVIOCGRAB`s the physical controller and re-emits a **virtual gamepad** (`28DE:11FF`, evdev
name "Microsoft X-Box 360 pad") — this is what our launcher reads at `/dev/input/event10` (confirmed
on-device). Consequences:
- **Standard controls** (ABXY, D-pad, L1/R1, L2/R2 analog, both sticks + L3/R3) arrive on the virtual
  pad → readable via raw evdev (current approach) or SDL2. ✅
- **Rear buttons L4/L5/R4/R5, trackpad touch/click, gyro** are **NOT** on the virtual pad — they reach
  an app **only via a Steam Input configuration** (`config/steam_input.vdf`). [Alice's blog](https://nyaa.place/blog/steam-deck-hid-and-libmanette-adventures/)
- **STEAM, QAM (…), volume, power are permanently system-reserved** — never design a feature needing
  them. [SteamOS #1050](https://github.com/ValveSoftware/SteamOS/issues/1050)
- **Haptics:** whole-controller rumble via `SDL_GameControllerRumble` (dual-motor; no trigger rumble on
  Deck). Trackpad haptics are Steam-Input-only — no portable evdev/SDL path.
- Node numbers are unstable across boots/resume — **enumerate devices by identity**, never hardcode
  `eventN`.

## 2. Canonical controller mapping (adopt PS/Xbox muscle memory; emit only keys Leanback needs)
| Deck control | Browse | In-player | Injected key(s) |
|---|---|---|---|
| D-pad + Left stick | Move focus | Scrub L/R on progress bar; details U/D | Arrow 37–40 |
| **A** (Cross-equiv) | Select | Play/Pause | `Enter` (13) |
| **B** (Circle-equiv) | Back / up level | Exit player | `Escape` (27) → fallback `Backspace`(8)/TV `461` |
| **X** | Context / details | Captions | `c` |
| **Y** | Open search | — | `/` |
| **L1 / R1** | Prev / next rail | Previous / Next video | `Shift+P` / `Shift+N` |
| **L2 / R2** | — | Seek −10s / +10s | `j` / `l` |
| Menu (☰) / stick-click | — | player menu (optional) | media keys optional |
| STEAM / QAM / vol / power | (OS-level) | — | not injected |

**Web Leanback key contract** (`youtube.com/tv`), the load-bearing detail since we inject keys:
- **Guaranteed** (verified on-Deck, S0.6): `ArrowUp/Down/Left/Right`, `Enter`, `Escape`(27).
- **`c` captions: CONFIRMED DEAD on Leanback (2026-07-22).** `c` is the *desktop* watch-page caption
  hotkey; youtube.com/tv receives the keydown and does nothing (the "best-effort" hope below was
  wrong — see §8.1: Google publishes no key contract for the TV app, so no desktop shortcut may be
  assumed). Captions were moved OFF a keystroke entirely: `toggle_captions` is now a **launcher
  action** driven over CDP (`config/scripts/toggle_captions.js`), which toggles the HTML5 player's
  own caption module (`getOption`/`setOption` `tracklist`+`track`) the same way `chapter_seek.js`
  drives seeking through the player API rather than a guessed key. The module name is version-
  dependent (`captions` on HTML5, `cc` on legacy AS3 — the script probes both), so the script is
  hot-swappable. **Implemented; on-Deck functional verification still pending.**

> **⚠ Correction 2026-07-09 (research pass 5) — this section previously mixed two incompatible
> keycode worlds.** See §8.1. Chrobalt is **Chromium**, so `KeyboardEvent.keyCode` is the **Windows
> virtual-key** set. The CEA-2014/HbbTV codes this doc used to list as fallbacks (`461` GoBack,
> `415` Play, `413` Stop, `412` RW, `417` FF) are **Tizen/webOS/HbbTV browser** codes and are *not*
> native to our engine. Likewise `j`/`l`/`k`/`0`–`9`/`/`/`Shift+N/P` are **desktop watch-page**
> shortcuts ([answer/7631406](https://support.google.com/youtube/answer/7631406)); Google publishes
> **no** key contract for the TV app, so none of them may be assumed. Nothing here changes what is
> *bound* today (we never shipped those), but the "treat 461 as a fallback" guidance was wrong and
> is withdrawn pending the §8.4 spike.

[YT Help: consoles](https://support.google.com/youtube/answer/3013387) ·
[Suitest TV keys (the *other* keycode world)](https://suite.st/docs/faq/tv-specific-keys-in-browser/) ·
[YT desktop shortcuts (NOT the TV app)](https://support.google.com/youtube/answer/7631406)

## 3. Touchscreen
- Device: Focaltech **`FTS3528` / input id `2808:1015`**, genuine multitouch (MT-B), `1280×800`. Its
  `eventN` **changes per boot** — enumerate by name/VID:PID/`ABS_MT_SLOT` capability.
  [crocidb](https://crocidb.com/post/investigating-touchscreen-issue-steam-deck/)
- Under gamescope, touch defaults to **single-touch pointer emulation** (a tap = a left click at that
  point) — so a tap already "clicks" a Leanback tile = activate, for free. True multitouch/gestures
  need per-game "Touch API Pass-through" and gamescope's multitouch story is limited; **don't rely on
  multitouch.** [Steamworks Deck FAQ](https://partner.steamgames.com/doc/steamdeck/faq)
  > **⚠ "For free" has an expiry date (§11).** It is free only for as long as we never *read* the
  > touchscreen. The moment we add a gesture layer we must grab the node, gamescope stops seeing
  > touch, and tap-to-activate becomes our code to write.
- **Optional richer touch layer (our own):** read the FTS3528 evdev directly in the launcher and
  translate to keys/gestures, borrowing YouTube-mobile's proven spatial model:
  **left-half / right-half / center** — double-tap left/right = seek ∓10s (`j`/`l`), center tap =
  toggle controls (`Enter`), tap-and-hold = temporary 2× (best-effort). Vertical brightness/volume
  swipes are **not** stock YouTube and are OS-level on Deck — skip. [YT mobile gestures](https://www.howtogeek.com/749519/5-youtube-gestures-you-should-be-using-on-android-and-iphone/)

## 4. ★ Runtime touch block / unblock (the explicit requirement)
> **SUPERSEDED 2026-07-10 → see [durable/touch-lock.md](touch-lock.md).** The prediction below —
> that an exclusive `EVIOCGRAB` starves gamescope of touch — was **physically tested on-Deck and is
> FALSE**: gamescope reads the FTS3528 panel by a path our grab does not intercept, so touch keeps
> flowing through a successful grab (and, as the seat user, the launcher cannot even open the panel
> node). Touch is instead made inert by `disable_touch` (default on): `no_pointer.js` swallows every
> pointer/mouse/touch event in the page **and** `touchmode.cpp` holds gamescope's
> `STEAM_TOUCH_CLICK_MODE` at hover while our window is focused — verified inert (0 clicks) on-Deck
> 2026-07-10. The analysis below is retained as the record of why the grab approach was tried; it is
> not open work.

**Mechanism:** open the FTS3528 node and `ioctl(fd, EVIOCGRAB, 1)`. gamescope/libinput read the
touchscreen *without* grabbing it, so our exclusive grab **starves them of touch system-wide**;
`EVIOCGRAB, 0` / `close(fd)` restores instantly and the kernel auto-releases on crash (fail-safe).
Stack-agnostic (below libinput/Xwayland), beats `xinput`/udev/gamescope-flags. Needs the Flatpak
`--device=input` permission (already granted via `flatpak override` in `just install`). Re-enumerate
by identity after resume. [evkill](https://github.com/Enteee/evkill) ·
[libinput --grab](https://wayland.freedesktop.org/libinput/doc/1.4.3/tools.html)

**Policy** (never a silent, total, mode-gated disable — the Switch accessibility anti-pattern):
- **Auto soft-suppress** transient palm/graze touches during active controller navigation and during
  fullscreen playback with chrome hidden — but a *deliberate* center tap still reveals controls
  (escape hatch).
- **Explicit user touch-lock:** a visible affordance + remappable controller chord; engage shows a
  lock glyph + toast + haptic; unlock requires a deliberate hold/two-step (so stray touches can't
  defeat it). **The lock removes touch only — the controller is never disabled** (Verified: default
  config must reach everything). Gate via a hot-swappable `app.json` flag (`block_touchscreen`).
- Never mix touch (absolute) and trackpad (relative) on one shared cursor → the Deck "previous-
  location click" bug. [Deck touch bug](https://steamcommunity.com/app/1675200/discussions/1/4338725580140497497/)

## 5. Design constraints (Valve Deck-Verified + 10-foot, our compliance target)
- **Default controller config reaches ALL functionality**; users must not tweak settings to get
  controller support. Ship a good `steam_input.vdf` + grid art.
- **Glyphs match the active device** (Deck/Xbox); no mouse/keyboard prompts while on controller.
  Track `currentModality` for cosmetics only — **never gate whether an input functions** (Valve's
  last-wins warning; accumulate, don't lock). [recommendations](https://partner.steamgames.com/doc/steamdeck/recommendations)
- **Text legibility:** char height ≥ **9px @1280×800** (aim 12px; TV scale prefers ~24px body).
- **Targets 44–48px**, TV safe-area margins, always-visible focus ring, one focused element, cheap
  single-step Back with no confirm dialog, comfortable D-pad auto-repeat (fast but not overshooting).
- **Text entry** = Leanback's own on-screen keyboard driven by D-pad (Arrows+Enter already work), plus
  voice search (Phase 5) — do not force per-character-only typing.

## 6. Implications for our code
- Current M1 (D-pad/stick→arrows, A→Enter, B→Escape, auto-repeat) is correct and on the right axis.
  Extend to the full §2 map; move the mapping into `config/app.json` (hot-swappable) instead of
  hardcoded. ~~make **B send Escape + Backspace/461 fallbacks**~~ — **withdrawn 2026-07-09 (§8.1):**
  `Backspace` *is* `Back` in Cobalt (`kSbKeyBack == kSbKeyBackspace == 8`), so that "fallback chain"
  is a footgun inside the OSK, and `461` is a foreign (HbbTV) keycode. B sends `Escape` only.
- Add a `TouchGuard` (EVIOCGRAB RAII) in the launcher's evdev layer; enumerate FTS3528 by identity;
  drive auto-suppress from the existing play-state poll (`player.cpp`) + a controller-chord lock.
- Rear buttons/trackpad/gyro + glyphs are a **Steam Input config** deliverable, not evdev.
- Optional: whole-controller rumble via SDL2 on focus/activation/seek-confirm (off by default).

## 7. Gap analysis vs mobile YouTube / tablets / hybrid handhelds (review 2026-07-09)
Compared §1–§6 and the as-built P3 layer against current mobile-YouTube gestures, the Switch
YouTube app (closest hybrid analog), and the PS5 app (the parity benchmark). Tasks filed in
TASKS.md P2/P3/P8. Everything below stays in the launcher/config layer — no engine patch.
- **Touch scrolling does not exist.** gamescope's pointer emulation gives taps only, and Leanback
  (remote-driven) has no pointer scroll — rails longer than a screen are untraversable by touch.
  This is the Switch YT app's #1 criticism (it shipped with no swipe support at all:
  [Nintendo Support](https://en-americas-support.nintendo.com/app/answers/detail/a_id/42563),
  [htxt review](http://htxt.co.za/2018/11/09/youtube-on-the-nintendo-switch-whats-it-like/)); a
  launcher flick→arrow-burst layer beats that benchmark. Same layer: **no touch Back path exists**
  → left-edge swipe = Escape.
- **Mobile flagship gestures with no equivalent here:** hold-anywhere-for-2× while held
  ([9to5Google](https://9to5google.com/2023/07/17/youtube-2x-gesture/)), user-configurable
  double-tap seek interval (5–60s), tap-hold + swipe-up filmstrip precise seek, chapter-aware
  seeking. Whether web Leanback exposes speed/chapters via key or player API = open on-Deck spike.
- **Two seek models:** Arrow keys = the console *scrub* model (official
  [YT console help](https://support.google.com/youtube/answer/3013387)); `j`/`l` = instant ±10s.
  Pick one on-Deck and keep SUPPORT.md honest about it.
- **The §2 table is contextual (Browse vs In-player) but the implementation is context-free.** The
  play-state poll can drive keymap layers. The OSK is a third context — Switch sets the handheld
  bar in-keyboard (Y=space, B=delete;
  [Miketendo64](https://miketendo64.com/2018/11/09/guide-what-can-i-do-with-youtube-on-nintendo-switch-and-what-are-the-controls/));
  B=Escape inside an OSK likely exits search instead of deleting.
- **Right stick is unused** (only R3-click); consoles use the second axis for fast list traversal.
- **The §4 lock policy is violated as-built** (silent, instant same-chord toggle, no feedback, no
  deliberate unlock) — feedback is policy, not polish. Reading the grabbed TouchGuard fd enables
  selective re-inject (deliberate center tap → CDP click) = the §4 escape hatch + palm rejection;
  a bare EVIOCGRAB alone cannot honor §4.
- **Missing surfaces:** first-run controls onboarding (View=captions, L3+R3 are unguessable; every
  console app / Verified title shows a one-shot overlay); a kiosk error/retry page (content_shell's
  desktop error page is the current failure state — also the natural R1 hotfix surface); a
  documented remap story (Steam Input's physical→xinput rebinding already provides full per-user
  remap without touching app.json).
- **"Latency ≤ PS5" is unmeasurable as stated** — log evdev-receipt→CDP-ack per key in the
  launcher; calibrate once against a 120/240 fps camera.
- **Post-1.0:** pinch/zoom-to-fill for the 16:10 panel (letterboxed 720p), mirroring mobile's
  pinch-to-fill. Belongs in the doc §10 stretch backlog, not v1.

---

# Part II — Text entry, the Deck's keyboard keys, and 10-foot conventions (pass 5, 2026-07-09)

## 8. Text entry & keyboard keys — the last unresearched surface

Text entry was the one input surface with no registered finding, and it is the surface where the
most received wisdom turned out to be wrong. Four sub-findings, in dependency order.

### 8.1 ★ Two incompatible keycode worlds — do not mix them
Chrobalt is **Chromium**, so `KeyboardEvent.keyCode` carries **Windows virtual-key (VK)** values.
Cobalt's `SbKey` enum *is* the VK set, so the SbKey→DOM mapping is an identity for these codes.
The CEA-2014/HbbTV codes that TV-development docs enumerate belong to **Tizen / webOS / HbbTV
browser** builds and are **not** our engine's codes. Confusing the two is why §2 was wrong.

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

`key.h:36` literally reads `kSbKeyBack = kSbKeyBackspace,` with an inline comment steering callers
to `kSbKeyEscape` for semantic back. Two consequences that change our plans:

- **`Backspace` IS `Back` at the platform layer.** So "make B send Escape + Backspace as a fallback"
  (old §2/§6) is not a safe belt-and-braces — and **"B deletes a character inside the OSK" is
  actively dangerous**: the same code may pop the search view. The §7 OSK question now has a strong
  prior, not a blank.
- **`scan_rewind` / `scan_forward` finally have candidate keys** (227 / 228). They are the two
  actions `app.json` deliberately leaves unmapped. `launcher/src/devtools.cpp:kKeys` has no entry
  for either, so the launcher **cannot currently dispatch them at all** — adding the table entries
  is a prerequisite to even spiking them.

Note the mapping is *enum*-verified, not *behavior*-verified: what Leanback's obfuscated JS actually
listens for is unknowable statically. Since our CDP path sets `windowsVirtualKeyCode` explicitly, we
*can* send 461 if we ever want to — it is simply not native, and no evidence says the app honors it.
[youtube/cobalt `starboard/key.h`](https://raw.githubusercontent.com/youtube/cobalt/main/starboard/key.h)
· tree: `cobalt/src/starboard/key.h`, `cobalt/src/ui/events/keycodes/keyboard_code_conversion_starboard.cc`

### 8.2 ★ Voice search is not a key — it is a platform service we do not have
`kSbKeyMicrophone` (12290) exists, but Cobalt routes voice search through the
**`SoftMicPlatformService` Starboard extension** (`MediaRecorder` + `SbMicrophone`), not a DOM key.
Injecting 12290 will almost certainly do nothing, and on the Deck there is no Starboard platform
service at all (S0.5: zero starboard symbols in the binary).

**So `y = voice_search` can never be a key dispatch.** (Full design + spike ladder: **§13**, which
supersedes the sketch below — a trusted synthetic *click* beats focus-nav.) The viable path is
Leanback's
own on-screen **soft-mic button** (D-pad + Enter) backed by the real `getUserMedia` mic the launcher
already auto-grants (P5). This *retroactively justifies* the refusal to bind `voice_search` and
converts it from "unknown key" to "wrong mechanism" — a design change, not a lookup.
[Cobalt voice_search doc](https://developers.google.com/youtube/cobalt/docs/gen/cobalt/doc/voice_search)

### 8.3 ★ The Deck's own keyboard keys, and the OSK trap
Only **three** system combos emit real keyboard keys to a focused app; everything else on the STEAM
chord is intercepted. Valve publishes this only as an on-device long-press overlay, so the table is
CORROBORATED across 5+ independent sources rather than first-party.

| Combo | Effect | Emits a key? |
|---|---|---|
| **STEAM + D-pad Right / Down / Left** | **Enter / Tab / Escape** | **yes — real keyboard keys** |
| STEAM + X | Summon OSK | opens OSK (see trap below) |
| STEAM + B (hold) · STEAM + R1 · STEAM + L1 (hold) | force-quit · screenshot · magnifier | no |
| STEAM + L2 / R2 · STEAM + right stick/pad | right/left mouse click · cursor | mouse only |
| STEAM + left stick U/D | brightness | no |
| **STEAM · QAM (…)** | always Steam menus | **never reach the app** |

- **⚠ The OSK trap.** For **non-Steam, non-"game-like" apps (Chrome and Discord are the cited
  examples)**, **STEAM+X in Game Mode can summon the _desktop_ OSK and soft-lock the session.** The
  reliable path is the **QAM ("…") → keyboard button**. Deckback is exactly this shape: a non-Steam
  Chromium shortcut. This must be in SUPPORT.md before anyone is told to type.
  [steam-for-linux#9117](https://github.com/ValveSoftware/steam-for-linux/issues/9117) ·
  [SteamOS#855](https://github.com/ValveSoftware/SteamOS/issues/855)
- **No auto-OSK on `<input>` focus under Xwayland.** gamescope raises the OSK for native Wayland
  clients via **text-input-v3**; there is "no standard way to interact with the on-screen keyboard"
  for X11 clients, which is also why `SDL_StartTextInput` is a no-op on Deck. We run
  `--ozone-platform=x11` (S0.4), so **the keyboard will never pop up by itself.** The clean fix is
  becoming a native Wayland client — a real engine/ozone change, not a setting, and it collides with
  the S0.4 result. [gamescope#668](https://github.com/ValveSoftware/gamescope/issues/668) ·
  [crbug 40113488](https://issues.chromium.org/issues/40113488)
- The Steamworks OSK APIs (`ShowFloatingGamepadTextInput`, which delivers by synthesizing OS key
  events and would therefore work over Xwayland) require linking Steamworks. **Deckback does not and
  will not.** So Valve's "auto-invoke an OSK whenever text input is required" recommendation is
  **structurally unmeetable for us** — state it openly rather than pretend.
  [ISteamUtils](https://partner.steamgames.com/doc/api/ISteamUtils)
- **Physical USB/BT keyboards do reach the focused Xwayland app** in Game Mode — but Game Mode
  **forces QWERTY scancodes**, ignoring the user's configured layout (the OSK respects it; the
  physical keyboard does not). [SteamOS#798](https://github.com/ValveSoftware/SteamOS/issues/798)
- **Steam Input can emit arbitrary keyboard keys**, including for non-Steam shortcuts, resolved by
  **scancode against the system layout**. Our `config/steam_input.vdf` is currently *pure xinput
  passthrough* — it uses none of this. It is a live fallback if CDP key injection ever fails, and it
  is also the per-user remap layer (§7).
  [Steam Input keyboard binds](https://steamcommunity.com/sharedfiles/filedetails/?id=3172761874)

### 8.4 What only an on-Deck spike can answer (bind nothing before these pass)
The no-guessing policy stands: every item below is *enum*-verified but *behavior*-unverified.
Instrument first — install a capturing listener, then inject one code at a time:

```js
window.__log = [];
['keydown','keypress','beforeinput','input','textInput','keyup'].forEach(t =>
  addEventListener(t, e => __log.push({t, key:e.key, keyCode:e.keyCode,
    tgt:e.target.tagName, val:e.target.value, prevented:e.defaultPrevented}), true));
```

1. **OSK erase semantics** — with the search field focused, inject `Backspace`(8), `Escape`(27),
   `Delete`(46) separately; after each, read the field value *and* whether the view popped. Settles
   whether 8 erases or navigates back (§8.1 predicts: navigates).
2. **Direct printable typing** — inject `'a'` (65, with `text:"a"`). Does `.value` gain "a"? Do
   `keypress`/`beforeinput`/`textInput` fire? Decides whether a real/BT keyboard can ever type into
   Leanback, or whether its grid OSK is D-pad-only. **Highest-value spike of the set.**
3. **Transport keys** — during playback inject 179/178/227/228/176/177 and colors 403–406, with CEA
   415/461 as *negative controls*. Produces the definitive bind-list for `scan_rewind`/`scan_forward`
   and prev/next.
4. **Soft-mic reachability** — confirm the mic button is D-pad-focusable and that activating it
   triggers `getUserMedia` with no prompt (P5 auto-grant).
5. **Speed / chapters** — no TV-app hotkey is documented; speed is a D-pad menu (Up → controls →
   settings → Speed). Inject the desktop candidates as negative controls before believing otherwise.

## 9. 10-foot guidelines on a 7" panel — the angular-equivalence rule

**The insight that makes TV guidance transferable.** Visual *angle*, not pixels, is what the eye
resolves. Screen-height / viewing-distance:
- 55" TV at 3 m → 686 mm / 3000 mm ≈ **0.229 rad**
- Steam Deck at 40 cm → 94 mm / 400 mm ≈ **0.235 rad**

They are within ~3%. **Therefore any 10-foot guideline expressed as a fraction of screen height
transfers to the Deck ≈1:1.** Guidelines expressed in *absolute pixels on a 1080p canvas* do **not**,
because we render 720p and letterbox. That single rule resolves most "is this TV advice valid here?"
questions, and the exceptions are what matter:

| TV guideline | On the Deck |
|---|---|
| TV-safe / overscan margins (~5%; Android TV 58dp/28dp) | **INVERTED — drop.** Overscan is a CRT/TV-hardware artifact; the Deck panel has none. Our own insets would waste scarce pixels. (Leanback applies its own server-side; we cannot remove those.) |
| Elevated contrast beyond WCAG 4.5:1 | **Keep** — but the reason changes from *distance* to *glare* on a glossy handheld panel used outdoors. |
| "TV-safe colors" / avoid banding-prone saturation | **Mostly drop** — a panel-handling limitation. OLED near-black smearing is the only residue. |
| Large typography (Fire TV ~28px @1080p floor) | **Transfers by angle** (~21px on our 800-tall buffer). The hazard is Leanback's *smallest* metadata text (~14–16px @1080p → ~11–12px here), which lands near Valve's floor and is rendered from a 720p buffer. **Measure the smallest rendered glyph on-device.** |
| Strong multi-cue focus ring (scale + border + glow) | **Transfers** — glare degrades a color-only ring even at 40 cm. |

**Valve's hard numbers** (the Deck-Verified gate): smallest on-screen character **≥ 9 px at
1280×800**, **12 px recommended**; default controller config must reach **all** functionality with
no in-app settings changes; no non-controller (keyboard/mouse) glyphs under the default config.
[Deck compat](https://partner.steamgames.com/doc/steamdeck/compat) ·
[recommendations](https://partner.steamgames.com/doc/steamdeck/recommendations)

**Two Verified requirements we structurally cannot meet, and must document rather than fake:**
1. **Glyph correctness** — Leanback renders its own generic/remote glyphs server-side. We do not
   control them and cannot map them to Deck glyphs.
2. **Auto-invoked OSK on text input** — requires Steamworks or Wayland text-input-v3 (§8.3).

**Adopted checklist** (each already sourced above; the ones we violate are filed as tasks):
- One focused element, always visible, multi-cue ring. *(Leanback's job; don't break it.)*
  [Android TV focus](https://developer.android.com/design/ui/tv/guides/styles/focus-system)
- Every direction from every element leads somewhere — no focus traps, no dead ends. Test every
  direction on every surface; this is the most common TV-app defect.
  [Android TV nav](https://developer.android.com/design/ui/tv/guides/foundations/navigation-on-tv) ·
  [Xbox](https://learn.microsoft.com/en-us/windows/uwp/xbox-apps/tailoring-for-xbox)
- Back is one predictable step, never behind a confirm dialog. **Handheld inversion:** on Android TV
  repeated Back lands on the OS launcher; in our kiosk it must land in **Steam Game Mode** — never a
  blank shell or Chromium chrome. Decide deliberately: swallow Back at the Leanback root, or quit.
- **D-pad auto-repeat is ours to own** (we synthesize the keys): ~300–500 ms initial delay, ~4–8
  repeats/s, configurable in `app.json`. Windows' 1000 ms/31 Hz default is wrong for list scrolling.
- Text entry: voice/dictation as a first-class path, not per-character D-pad typing; preserve
  Leanback's phone-pairing flow; QWERTY over alphabetical.
  [tvOS HIG](https://developer.apple.com/design/human-interface-guidelines/focus-and-selection) ·
  [Android TV OSK](https://developer.android.com/training/tv/get-started/onscreen-keyboard)
- Errors: an in-app, **controller-focusable Retry** — never a Chromium `net::ERR_*` interstitial. In
  a kiosk with no address bar and no keyboard, a browser error page is a hard dead-end.
  [Roku principles](https://developer.roku.com/dev/docs/key-design-principles)
- First-run: one-shot, controller-dismissable, re-openable controls overlay.
  [Apple onboarding](https://developer.apple.com/app-store/onboarding-for-games/)

## 10. What pass 5 changes in our code and docs
- `devtools.cpp:kKeys` must gain **`MediaRewind` (227)** and **`MediaFastForward` (228)** before
  `scan_rewind`/`scan_forward` can even be spiked. It currently cannot dispatch them.
- **Withdraw** the "B sends Escape + Backspace/461 fallback" guidance (§2/§6). `Backspace == Back`
  in Cobalt; a fallback chain that includes it is a footgun inside the OSK, and 461 is foreign.
- `y = voice_search` is **not a keymap problem** (§8.2). Re-scope it to soft-mic-button focus.
- SUPPORT.md **must** tell users to open the OSK from the **QAM menu, not STEAM+X** (soft-lock), and
  state that the keyboard never auto-appears, and that a physical keyboard is forced to QWERTY.
- Deck-Verified glyph correctness + auto-OSK are **structurally unmeetable**; say so in SUPPORT.md /
  README rather than claiming Verified parity.
- Auto-repeat defaults (350 ms / ~7.7 Hz today, `input.cpp`) already sit inside the recommended band;
  the *acceleration* curve remains unexercised on-device.
- No engine patch is implied by any of the above — everything stays in `launcher/` + `config/`.

## 11. Touch is all-or-nothing — the hidden price of a gesture layer (2026-07-09)
> **SUPERSEDED 2026-07-10 → see [durable/touch-lock.md](touch-lock.md).** Moot: the grab does not
> block gamescope's touch at all (proven on-Deck), so there is no grabbed node to build a gesture
> layer on. Touch is handled by `disable_touch` (page swallow + gamescope hover-lock), not a grab.
> The trade-off reasoning below stands as analysis of a path we did not ship.

`EVIOCGRAB` is exclusive: a grabbed evdev node delivers events **only** to the grabbing client. There
is no "observe without stealing". That gives exactly two ways to see touch, and both have a cost that
§3 and the §7 gesture task quietly assumed away:

| Approach | We see gestures? | gamescope sees touch? | Consequence |
|---|---|---|---|
| **Grab + read** | yes | **no** | We must re-synthesize *everything*, tap-to-click included |
| **Read without grab** | yes | yes | **Double actuation** — our double-tap-seek *and* gamescope's click both fire |

The second is not a design, it is a bug: any gesture that starts with a finger-down overlaps the tap
gamescope is already turning into a click. **So a gesture layer forces the permanent-grab path**, and
with it:

- **Tap-to-activate becomes ours.** The "free" tile activation in §3 disappears; we own hit-testing,
  re-injecting taps as CDP `Input.dispatchMouseEvent` clicks at the right coordinates, and getting
  the 720p-letterboxed-into-800p coordinate transform right.
- **Palm rejection becomes ours**, since nothing else is filtering.
- **A launcher crash now kills touch entirely** until the kernel auto-releases the grab. Today a
  crash while unlocked is harmless.

This is also the **enabling** fact for the §4 policy: the deliberate-center-tap escape hatch and
auto soft-suppress are *only* implementable on the grab+read path. A bare `EVIOCGRAB` with no reader
— which is what `touch.cpp` does today — can never honor §4. Lock and gestures are the same
mechanism, and they must be designed together or not at all.

**Current code gap (`launcher/src/touch.cpp`).** `resolve_and_open()` opens `O_RDONLY` and
`set_blocked(true)` grabs, but **the fd is never read**. So the lock is total:
- no escape hatch, no palm rejection (§4 policy violated as-built, per §7);
- the lock is no longer *silent* — see §14 — but the two remaining §4 obligations
  (deliberate-center-tap escape, auto soft-suppress) still require the grab+read path this section
  prices out.

**Recommendation:** treat the gesture layer as a *deliberate architectural commitment*, not an
incremental feature. If the price is not worth paying, the cheaper win is §12.

## 12. The seek model is contradictory as shipped, and no verified key can fix it

Three artifacts disagree about what L1/R1 do:

| Artifact | Claim |
|---|---|
| `docs/SUPPORT.md` | "**L1 / R1** — Seek back / forward 10s" |
| `config/app.json` | `lb: seek_back_10`, `rb: seek_fwd_10` |
| `launcher/src/input.cpp` | `{"seek_back_10", "ArrowLeft"}`, `{"seek_fwd_10", "ArrowRight"}` |

Arrow keys in the Leanback player are the console **scrub** model, not an instant ±10 s jump. So the
alias *name* is wrong, and SUPPORT.md promises a behavior the code does not implement. The instant
±10 s keys (`j`/`l`) are **desktop watch-page** shortcuts (§8.1) — not a TV contract, and
`devtools.cpp` cannot dispatch them anyway.

Consequently **no input mechanism can deliver "seek ±10 s" today** — not touch double-tap, not a
trigger layer. The blocker is the *action*, not the input. Fix the naming and the doc now; bind a
real seek only after spike §8.4 #3.

**Corollary — L2/R2 are dead buttons and the cheapest thing we own.** `lt`/`rt` map to
`scan_rewind`/`scan_forward`, which resolve to nothing, so the triggers currently dispatch nothing at
all. Meanwhile `trigger_pressed()` already implements press/release hysteresis and `lt_down_`/
`rt_down_` already track **held** state — precisely what a modifier needs. Repurposing L2/R2 as a
held "shift" layer (the context-layer task in §7) costs no existing functionality, needs no grab, no
re-injection, no palm rejection, and works with the touchscreen locked. It is strictly cheaper than
the gesture layer and should land first.

*Hazard to check on-device before relying on it:* `config/steam_input.vdf` binds the triggers as
`"click" → xinput_button TRIGGER_LEFT/RIGHT` (a digital press), while `input.cpp` reads the **analog
axes** `ABS_Z`/`ABS_RZ`. Whether Steam's virtual pad still emits the analog axis under that binding
is **unverified**; if it emits a digital button instead, the hysteresis path never executes and the
triggers stay dead for a second, entirely different reason.

## 13. Voice input — hold-to-talk, and why it cannot be a key (design, 2026-07-09)

The Deck has a dual mic array and the launcher already auto-grants `audioCapture` to the app origin
(P5, `navigator.cpp`). The product ask is the TV convention: **hold a button, speak, release.**

### 13.1 The mechanism is a click, not a keypress
From §8.2: `kSbKeyMicrophone` (0x3002) exists, but Cobalt drives voice through the
**`SoftMicPlatformService` Starboard extension** (`MediaRecorder` + `SbMicrophone`), and S0.5 found
**zero starboard symbols** in this Chromium-based binary. **Injecting 12290 will do nothing.** There
is no key to bind, at any keycode, ever. Anything that says otherwise is guessing.

The only path is to **activate Leanback's own on-screen soft-mic button**. Two ways:

| | How | Verdict |
|---|---|---|
| (a) focus-nav | D-pad focus onto the mic button, then `Enter` | Fragile — depends on where focus currently is, and on the search view being open |
| (b) **trusted synthetic click** | `Runtime.evaluate` → `getBoundingClientRect()`; `Input.dispatchMouseEvent` at its centre | **Preferred.** Position-independent, and CDP mouse events are trusted like its key events |

A convenient property: **CDP mouse coordinates are CSS pixels of the viewport**, so
`getBoundingClientRect()` feeds `dispatchMouseEvent` directly — *no* letterbox transform. (Contrast
§11: raw touchscreen evdev coordinates *would* need one. Voice is the cheap half of that machinery,
and building it first de-risks the gesture layer.)

`devtools.cpp` has no `dispatchMouseEvent` today — it is key-only. That is the first code gap.

### 13.2 ★ The underrated risk: our UA may hide the mic button
We ship a **Cobalt** UA. Leanback may therefore assume the platform provides voice via
`h5vcc`/`SoftMicPlatformService` and either (a) not render a web soft-mic button at all, or (b)
render one whose handler calls a platform API that does not exist here. **Our own identity bet (R1)
could be what breaks voice.** This is the first thing to check, before any button is bound:

```js
({ h5vcc: typeof window.h5vcc,
   mic: !!document.querySelector('[aria-label*="voice" i],[aria-label*="search by voice" i],[class*="mic" i]') })
```

If Leanback gates voice on `h5vcc`, there is a **patch-free hypothesis** worth testing: shim the
platform service in an injected script via `Page.addScriptToEvaluateOnNewDocument` — the exact
technique `navigator.cpp` already uses for AV1 steering — advertising a soft mic with
`micGesture: "HOLD"`. The service's JSON protocol is Cobalt-internal and undocumented, so treat this
as an **unverified hypothesis, not a plan**. If it fails, voice search is not deliverable on this
engine and we say so rather than shipping a dead button.

### 13.3 Speaker bleed — the handheld-specific problem
Hold-to-talk while a video plays feeds the Deck's own speakers into its own mics, ~15 cm away. TVs
solve this by ducking. We already own `PlayerController`, so: **pause or duck playback for the
duration of capture, and restore on release.** This is not polish — it likely determines whether
server-side ASR returns anything usable. Any voice spike that tests in silence has not tested the
real case.

### 13.4 Which button
`y` is already nominally `voice_search`, is unbound, and is where TV apps put search — **that is the
default.** For a deliberately "unpopular" button, note the constraint from §1: **rear grips
L4/L5/R4/R5 are not on the virtual pad** and cannot be read by evdev. The only route is a Steam Input
binding that maps a grip to some *standard* xinput button the launcher then reads — and our
`steam_input.vdf` currently maps all four grips onto face buttons, so a grip is today
indistinguishable from the face button it duplicates. Freeing one (e.g. `START`, whose `player_menu`
action is dead anyway) is the prerequisite for putting voice on a grip.

Hold semantics need a real code change: `input.cpp` dispatches on the **press edge only**
(`if (value != 1) return;`) and discards releases for mapped buttons. Hold-to-talk needs press→start,
release→stop, with a short debounce so a stray tap does not open the mic.

### 13.5 Spike ladder — stop at the first failure
1. **`h5vcc` present? mic button in the DOM under our UA?** (§13.2). If no button and no shim, stop.
2. Is the button focusable/activatable at all (D-pad + `Enter`)?
3. Does a **trusted `Input.dispatchMouseEvent`** on its rect start capture — `getUserMedia` called,
   **no permission prompt** (proves the P5 auto-grant, still unverified on-Deck)?
4. Does the page honor **hold** (`micGesture: "HOLD"`), or is it tap-to-toggle? If toggle, emulate
   hold in the launcher: click on press, click again on release.
5. Is the captured track **live and non-silent** through the Flatpak's pulse socket? (Assert on
   audio levels, not on the absence of an error.)
6. **Speaker bleed:** does ASR still resolve with a video playing? Compare ducked vs not (§13.3).

Nothing gets bound until 1–3 pass. Per the standing rule: a dead button is worse than a missing one,
because it teaches the user the feature is broken rather than absent.

## 14. Touch-lock feedback — making the lock observable, and the asymmetry of lock vs unlock (2026-07-09)
> **SUPERSEDED 2026-07-10 → see [durable/touch-lock.md](touch-lock.md).** The feedback here existed
> to make a lock observable, but the lock itself is dead (the grab does not block touch, proven
> on-Deck), so there is nothing to make observable. The toast/haptic primitives still exist but no
> longer gate touch. `disable_touch` needs no engage/disengage affordance — it is simply on.

§4 asks for "a visible affordance … engage shows a glyph". Until now `toggle_touch_lock()` printed a
log line and nothing else. That is not a cosmetic gap. **A locked touchscreen and a hung browser are
indistinguishable from the user's seat**: both are a panel that stopped responding to a finger. The
lock's own failure mode is therefore a bug report about something else entirely.

**Shipped (`launcher/src/overlay.{hpp,cpp}`, `launcher/src/haptic.{hpp,cpp}`, `input.cpp`):**

- **Toast.** A `<div id="__deckback_toast">` injected over CDP `Runtime.evaluate` and appended to
  `document.documentElement` (not `body` — Leanback replaces body content on navigation). No engine
  patch, same technique as the codec steering. `pointer-events:none`, so it can never swallow a tap.
  The lock toast names the chord: *"Touchscreen locked / Hold l3+r3 to unlock"*.
- **Haptic.** evdev `EVIOCSFF` + `FF_RUMBLE` on the pad. Chosen over SDL2 to hold the dependency
  budget (doc §13.2). It needs a second, **`O_RDWR`** fd — the input layer's are `O_RDONLY` — opened
  lazily on the first pulse, since no device is discovered at construction. A pad without `EV_FF` is
  ordinary, not an error: log once, toast only. Lock and unlock use different magnitudes, because a
  user who trips the chord without looking must still be able to tell which way it went.
- **Deliberate unlock.** `TouchLockChord` (pure, clock-injected). **Locking is immediate; unlocking
  requires holding the chord for `touch_lock_unlock_hold_ms` (default 800 ms).** The chord is two
  stick clicks, which a resting thumb finds by accident — and an accidental unlock hands the panel
  straight back to the palm already on it, which is the exact event §4 exists to prevent. Locking is
  cheap to undo; unlocking is not. Symmetry here would be a bug. One action per engagement, so
  continuing to hold after it locks does not then unlock.

`touch_lock_toast` / `touch_lock_haptic` / `touch_lock_unlock_hold_ms` are in `app.json`, so a user
can restore the old instant toggle (`0`) or silence either channel.

**RESOLVED 2026-07-10 (the worse-than-silence fear came true, then was routed around).** The grab
test ran: `EVIOCGRAB` on the FTS3528 does **not** starve gamescope — gamescope reads the panel by a
path the grab does not intercept, so the lock was exactly the silent no-op announcing a false state
that this section warned about. The whole grab-based lock is dead; touch is now handled by
`disable_touch` (page swallow + gamescope hover-lock), which needs no feedback because it is simply
always on. See [durable/touch-lock.md](touch-lock.md). (Original open-work note preserved below.)

> ~~**Still unverified, and it subsumes all of the above.** Whether `EVIOCGRAB` on the FTS3528
> actually starves gamescope of touch is untested on hardware (TEST-PLAN §2; the grab test is the
> first item in the on-Deck sequencing). If gamescope grabs the panel first, the lock is a silent
> no-op and this feedback announces a state that never engaged — worse than the silence it replaced.
> The grab test gates the feature, not the feedback.~~

One of the two smaller doubts is now **RESOLVED 2026-07-10**: a CDP-injected `<div>` *does* survive
and render over real Leanback — but only after adding a **Trusted Types policy**, because
`youtube.com/tv` enforces Trusted Types and a bare `innerHTML` assignment is a **silent no-op**
there (m114.md §"controls card / toast over real Leanback", ~685-712). Still unverified: that the
Steam Input virtual pad exposes `FF_RUMBLE` on a node we open `O_RDWR` inside the Flatpak sandbox
(`--device=input` is a filesystem permission, not proof the virtual pad implements FF).

**Reused by:** the first-run controls overlay (P2) is the same toast machinery with a longer timeout.

## 15. Right-stick fast traversal — why its ramp must NOT look like the D-pad's (2026-07-09)

§7 filed "right stick is unused; consoles use the second axis for fast list traversal". Shipped in
`fast_scroll()` (`input.cpp`, pure + unit-tested): the right stick emits the same arrow keys the
D-pad does, resolved through the same layer/modifier machinery, so it introduces **no new key
assumption** — arrows are the only Leanback bindings we have actually verified on hardware.

**The design decision is the ramp, and the two sticks must ramp differently.**

| | Signal | Correct ramp |
|---|---|---|
| D-pad / left stick | digital: "held" or not | accelerate over **time** — nothing else is available |
| Right stick | analog deflection | rate **is** deflection — the thumb is the accelerator |

Putting a time ramp on the right stick would mean two controllers driving one variable: the user
pushes harder to go faster while the ramp independently decides the same thing. Conversely, using
only a time ramp would make the right stick a slow D-pad and waste the axis. So: `slow_ms` (200) at
the deadzone edge, linear down to `fast_ms` (45) at full deflection, re-read from the *current*
deflection on every step (easing off slows the next arrow, not the next event).

Three non-obvious constraints the implementation must respect, each of which is a unit test:

- **`fast_ms` is a floor, not a target.** Each arrow is a `keyDown` + `keyUp`, both awaited over the
  CDP socket. Below ~25 ms the input thread lives in the socket and stops draining evdev. A
  hot-swapped `app.json` is untrusted input, so the floor is enforced in code, not documented.
- **The axis minimum is `-32768`, one past the `+32767` maximum.** Unsaturated, `|-32768| > |32767|`,
  so pushing the stick into its physical *down-left corner* would resolve **Left** while the
  down-right corner resolves **Down**. Saturate before comparing.
- **The dominant axis wins, vertical takes the tie** — unlike the digital `resolve_direction()`,
  where vertical always wins. Magnitude is meaningful here, so a diagonal push means whichever way
  the thumb pushed harder. Both rules agree on a perfect diagonal.

Suppressed while the D-pad or left stick hold a direction: one physical intent must never emit two
arrow streams.

**Unverified on hardware** (TEST-PLAN §2): that Steam Input's virtual pad emits `ABS_RX`/`ABS_RY` at
all under our `steam_input.vdf` (the same class of doubt as the L2/R2 trigger-axis hazard in §12 —
a `"click"` binding would give a button, not an axis), and that Leanback's rails accept a 45 ms arrow
cadence without dropping keys. Both fail *safely*: no axis means no scrolling, and dropped keys mean
a slower scroll. Neither can dispatch a key we did not intend.

We also normalise against the Xbox-pad range rather than the device's `absinfo`, because the merged
multi-device path has no single authoritative range. A narrower-range pad scrolls slower than
intended, never faster.

## 16. The kiosk failure state, and a silent bug in how we detected it (2026-07-09)

§7 filed "a kiosk error/retry page; content_shell's desktop error page is the current failure state".
Implementing it surfaced something worse than a missing page.

**The bug.** `Page.navigate` reports a *failed* navigation as a **successful CDP command** carrying an
`errorText` field — it is not a CDP error, and `request()` returning a body proves nothing. Worse, the
failed document keeps `document.location.href` set to the URL we asked for. The Navigator's liveness
check was `document.location.href contains "www.youtube.com/tv"`, so on a network outage it concluded
**the TV app had loaded** and stopped re-navigating. The user sat on Chromium's desktop error
interstitial — a page whose only control is a mouse-clickable Reload button, in an app with no
address bar, no keyboard, and no cursor — until they killed it from the Steam overlay. Nothing logged.
This is the state a dropped Wi-Fi connection puts every user in, and no test could have caught it,
because every observable we had said "fine".

The fix is `DevToolsClient::navigate_checked()` returning `{sent, error_text}`. The two failures are
different and must not be conflated: **`sent == false` is transport** (the engine is gone — retry the
socket, there is nothing to draw on), **`error_text != ""` is navigation** (the engine is fine, the
network is not — draw a page).

**The page** (`errorpage.{hpp,cpp}`). `about:blank` + a CDP-injected document. Not a bundled
`file://` page (one more sandbox path to get wrong, and it can fail with the very error it reports)
and not a `data:` URL (restricted as a top-frame navigation). `about:blank` always commits, even when
the network is what failed.

- A **focused** "Try again" button with a visible focus ring. On a controller there is no cursor, so
  an unfocused button is an unreachable one — precisely the failure being replaced. Enter and Space
  retry.
- **Escape is deliberately unbound.** The only thing it could do is quit, and quitting the app
  because the Wi-Fi blinked is not a kindness.
- Automatic retry with exponential backoff (2 s → 30 s). A user-requested retry **resets** the
  backoff: they presumably just fixed the network, and making them wait 30 s to find out is hostile.
- The retry signal runs page → launcher (`window.__deckbackRetry`, read-and-cleared in one round
  trip). The launcher owns *when* to retry, because only it knows whether the navigation succeeded;
  the page only knows a button was pressed.
- `classify_net_error()` maps the `net::` code to advice a person can act on, and **refuses to guess**
  on anything it does not recognise. Telling someone to check their Wi-Fi when their certificate
  expired sends them to fix the wrong thing; a vague honest hint beats a confident wrong one.

**This is the R1 hotfix surface.** `error_title`/`error_hint` ship in `app.json`. If Leanback changes
under us and stops loading, the explanation that reaches users is a config push, not a rebuild. Both
strings are escaped on the way into the page — a quote in hot-swapped text would otherwise close the
JS string literal and the page would silently never render.

**Unverified on hardware**: that a CDP-injected document survives on `about:blank` under gamescope,
and that our arrow/Enter injection reaches a button on our own page the same way it reaches Leanback.
Both are testable at L2 the moment it exists: pull the network, assert the Retry button has focus.

## 17. The first-run controls card, and why it must not be written in C++ (2026-07-09)

§7 filed "first-run controls onboarding (View=captions, L3+R3 are unguessable)". Two of this app's
controls appear nowhere in Leanback's UI, and `docs/SUPPORT.md` and `install.sh` are invisible from
Game Mode. Every console app and every Deck-Verified title ships a one-shot controls card.

**The card is generated from `app.json`, never hardcoded.** This is the whole design. `app.json` is
built to be hot-swapped remotely (doc §6 R1) — that is our answer to Leanback breaking under us — so
a card with "View = Captions" baked into C++ would keep saying it after a hotfix rebound View. A
controls card users cannot trust is *worse* than no card, because they stop reading it. Two rules
follow, both unit-tested against the real shipped `config/app.json`:

- **A control that resolves to no DOM key is omitted.** Today `L2`/`R2` (`scan_rewind`,
  `scan_forward`) dispatch nothing. Listing them teaches the user the app is broken rather than that
  the feature is absent — the dead-button failure from §13.
- **A feature that ships off is not advertised.** Voice is `voice_enabled: false`, so no "Hold to
  speak" row appears, even though `y` is bound and waiting.

The two launcher-performed actions (`voice_search`, `show_controls`) have no DOM key *by design*, so
they are recognised explicitly rather than dropped by that rule — otherwise the card would omit the
one row explaining how to reopen itself.

**`start` was rebound from `player_menu` to `show_controls`.** `player_menu` resolves to no key: the
Menu button dispatched nothing at all, verifiably. Replacing a proven no-op with a working control is
non-destructive, and it is the only way to satisfy §7's "re-openable". If an on-Deck spike ever
establishes a real `player_menu` key, `show_controls` moves to whatever is dead then. It must never
displace a working binding.

**The card is modal, and the launcher — not the page — enforces it.** While it is up, `GamepadInput`
swallows every event. Without that, a D-pad press would move Leanback's focus behind a card the user
cannot see through, and the first thing they would do after dismissing it is wonder where they are.
**Only a button dismisses, never a stick or D-pad**: an analog stick drifts at rest, and a card
dismissed by a resting thumb before it is read was never shown.

The marker is written **on show, not on dismiss**, and is versioned (`first_run_v1`). On dismiss
would mean the card reappears forever for anyone whose app is killed (or crashes) during onboarding;
versioned means a materially changed keymap can show the card once more rather than hiding behind a
marker written years earlier. It lives in `$XDG_STATE_HOME/deckback/`, deliberately not in the
Chromium profile, which is Chromium's to manage.

**Render RESOLVED 2026-07-10**: the injected card *does* render over real Leanback, once a
**Trusted Types policy** is used (a bare `innerHTML` is silently dropped on `youtube.com/tv` — see
§14 and m114.md ~685-712). **Still unverified on hardware**: that swallowing events at the evdev
layer really does keep focus still behind the card — the CDP-injected keys are the only ones we
suppress, but Steam Input could in principle deliver a second path we do not see. That is an L2
test: show the card, press D-pad, assert `document.activeElement` did not move.

## 18. ★ Seek spike (§8.4 #3 / §12): `player.seekBy` works, chapter data does NOT (on-Deck, 2026-07-13)

The seek spike §12 deferred ("bind a real seek only after spike §8.4 #3") ran on-Deck against the
running app over CDP (`scripts/cdp.py` through an `ssh -L 9222` tunnel; five `Runtime.evaluate`
passes). It answers §12's blocker in **both** directions, and kills a feature idea before it cost
any code.

**Verified positive — the first real TV seek primitive.** Under the TVHTML5 UA
(`navigator.userAgent = …Cobalt/26.lts.0`), `document.querySelector('#movie_player')` **exists** and
carries a live player API: `getPlayerResponse()`, **`seekTo(sec, allowSeekAhead)`**, and
**`seekBy(deltaSec, allowSeekAhead)`** are all present (method probe returned
`seekTo, seekBy, seekToLiveHead, requestSeekToWallTimeSeconds, seekToStreamTime`). So §12's claim
that *"no input mechanism can deliver seek today"* is now **false for a fixed-interval skip**:
`#movie_player.seekBy(±10, true)` is a verified, TV-native jump. This is the mechanism to bind to the
dead L2/R2 triggers (§12 corollary) — **but it is a JS eval, not a DOM key**, so it needs a new
binding kind in `keymap.cpp` (an action that calls `client_.eval_void(...)` rather than
`dispatch_key`), not another `kActionAliases` row.

**Verified negative — chapters are unreachable on this client, so chapter-aware seek is out.** The
idea was L2/R2 jump to the prev/next YouTube *chapter*. The data is not obtainable on TVHTML5 without
adopting yt-dlp-grade extraction fragility:

| Source probed (video `aircAruvnKk`, has chapters) | Result |
|---|---|
| `getPlayerResponse().playerOverlays…multiMarkersPlayerBarRenderer` (desktop chapter path) | **absent** — `playerOverlays` is undefined on TVHTML5 |
| recursive walk (depth 9) of the whole player response for `/chapter|marker/i` | **zero hits**; no chapter method on the player object either |
| `videoDetails.shortDescription` (description-timestamp fallback) | **empty string** — TVHTML5 strips the description too |
| out-of-band `POST /youtubei/v1/player` forced to a **WEB** client context (same-origin, page's `INNERTUBE_API_KEY`, `status 200`) | **`playabilityStatus: UNPLAYABLE`, no `playerOverlays`** — for `aircAruvnKk`, `dQw4w9WgXcQ` (Rick Astley — unquestionably playable), and `QpBTM0GO6xI` alike |

The InnerTube client is `TVHTML5` (`ytcfg` `INNERTUBE_CONTEXT.client.clientName`), whose player
response is a minimal variant (`responseContext, playabilityStatus, streamingData, captions,
videoDetails, playerConfig, storyboards, endscreen, …` — no overlays, no description). Spoofing a
WEB context from the app is rejected: **every** id came back UNPLAYABLE, including a guaranteed-good
one, which means the *request context* is refused, not the videos — 2026-era YouTube gates the WEB
client behind fresh client-versions + `visitorData`/proof-of-origin tokens. Getting chapters would
mean tracking YouTube's client versions and tokens the way yt-dlp does: an arms race squarely
against our minimal-fragility / R1 posture. ~~**Do not build chapter-aware seek.**~~ Build fixed-interval
`seekBy` skip instead, and if you want "chapter-ish" navigation later, revisit only if the TVHTML5
`/next` engagement-panel path (unprobed) turns out to carry `macroMarkersListEntity`.

**★ CORRECTION 2026-07-13 (on-Deck, live app over CDP) — the `/next` lead pays off; chapters ARE
reachable.** The negative table above only tried `/player` (no overlays) and a WEB-context `/player`
(UNPLAYABLE). It never tried **`/next`**, the exact endpoint flagged one line up as the unprobed lead.
It works: `POST /youtubei/v1/next` with the page's TVHTML5 context + `INNERTUBE_API_KEY` and the
videoId passed directly (`status 200`) returns an `engagement-panel-macro-markers-description-chapters`
panel AND a `macroMarkersListEntity` in `frameworkUpdates.entityBatchUpdate.mutations[].payload`. Its
`markersList` (`markerType: MARKER_TYPE_CHAPTERS`) is `markers[]`, each with `title.simpleText`,
**`startMillis`** (string ms), `durationMillis`, and
`onTap.innertubeCommand.seekToVideoTimestampCommand.offsetFromVideoStartMilliseconds`. Confirmed for
`X5xlTpuiRCE` (0 / 348000 / 533000 / 638000 …). Milestone-keyed shape lives in m138.md.

**★ The TVHTML5 player element is `.html5-video-player`, NOT `#movie_player`.** `#movie_player` is a
desktop-watch-page id and does NOT exist on youtube.com/tv; the player API object there is
`document.querySelector('.html5-video-player')` (id `ytlr-player__player-container-player`), which
exposes the full API — `seekTo`, `getCurrentTime`, `getDuration`, `getVideoData`, `seekBy`,
`getPlayerState`. The first cut of skip.js/chapter_seek.js keyed on `#movie_player` and so silently
fell through to the raw `<video>` element (skip still nudged `currentTime`, but chapter_seek never
reached its chapter branch — "just skips, not by chapters"). All player scripts now select
`.html5-video-player || #movie_player || video`. Verified on-Deck 2026-07-13. **Any future player
interaction must use this selector, not `#movie_player`.**

**Implemented + on-Deck verified 2026-07-13.** `config/scripts/chapter_seek.js`
(ScriptLibrary, params `dir`/`skip`) does exactly the design below; `chapter_action_sign()` in
`keymap.cpp` recognises `chapter_back`/`chapter_fwd`; `input.cpp` prebuilds the render per trigger.
`config/app.json` ships `lt`→`chapter_back`, `rt`→`chapter_fwd` (the new default) — `skip_back`/
`skip_fwd` remain for pure fixed-jump. Non-blocking: `eval_void` does not await promises, so the
script caches chapters on `window.__dbChapters` and fixed-skips on a cache miss while a background
`/next` fetch warms it (first chapter press after a new video acts as a skip; subsequent are true
chapter jumps). No launcher/PlayerController change — all logic in the one hot-swappable script.
L0-tested (`input_test.cpp`: `chapter_action_sign`, the render shape). Design as built:

**How chapter-aware L2/R2 works:**
1. On entering a watch view (PlayerController already detects the `#/watch` route), fetch `/next` for
   the current videoId via a page-context script (`config/scripts/chapters_fetch.js` — ScriptLibrary,
   so it is hot-swappable when Leanback shifts the shape), parse `startMillis[]` where
   `markerType == MARKER_TYPE_CHAPTERS`, sorted. Cache per videoId; refetch on video change.
2. L2/R2 → prev/next boundary: read `#movie_player.getCurrentTime()`, find the surrounding chapter,
   `#movie_player.seekTo(targetSeconds, true)` (verified primitive). "Prev" jumps to the current
   chapter's start unless within ~2 s of it, then the previous — the console convention.
3. **Fallback stays fixed-interval skip** when a video has no chapters (`markers` absent/empty) or the
   fetch fails — so L2/R2 always do *something*. This is the config knob split: `skip_back`/`skip_fwd`
   (today) vs a new `chapter_back`/`chapter_fwd` action that falls back to skip.
This is more "proper" than the desktop render-tree scraping rejected above: it uses the TV client's own
InnerTube data source, not internal JSON of a different client. Fragility is the usual R1 UA risk, no
worse than the rest of the app; the hot-swappable fetch script is the mitigation.

This does NOT change what SHIPPED: L2/R2 today do fixed ±10 s skip (verified working on-Deck
2026-07-13 — the triggers arrive as analog `ABS_Z`/`ABS_RZ`, the axis question below is RESOLVED).

*Milestone-scoped values* (exact keys/versions, keyed to cobalt-27/M138) belong in `m138.md`; the
strategic conclusions above are durable. *RESOLVED 2026-07-13 on-Deck:* the §12 hazard is gone —
Steam's virtual pad emits the **analog axis** (`ABS_Z`/`ABS_RZ`, which `input.cpp` reads), not a
digital button, so the press-edge path fires and L2/R2 seek works with the shipped code. No
button-path fallback was needed.

**Implemented + hardware-verified 2026-07-13.** The fixed-interval `seekBy` skip is built and
confirmed working on-Deck (OLED, live Leanback playback). `skip_action_sign()` in `keymap.cpp`
recognises `skip_back`/`skip_fwd`; `input.cpp` prebuilds `config/scripts/skip.js` (ScriptLibrary,
hot-swappable) per trigger at construction and `eval_void`s it on the press edge, taking precedence
over any DOM-key binding. Interval is `config:skip_seconds` (default 10). `config/app.json` ships
`lt`→`skip_back`, `rt`→`skip_fwd`, replacing the dead `scan_rewind`/`scan_forward`. L0-tested
(`input_test.cpp` + `scripts_test.cpp`). On-Deck: L2 = −10 s, R2 = +10 s, both seek the player. The
analog-axis question is RESOLVED (above). **Next iteration:** promote to chapter-aware seek via the
`/next` `macroMarkersListEntity` data (see the ★ CORRECTION above), keeping fixed skip as the
no-chapters fallback.

**★ BUGFIX 2026-07-14 — off-watch seek/resume resumed the backgrounded player (audible prev video).**
The seek scripts locate the player by `document.querySelector('.html5-video-player')` alone. Leanback
keeps that element in the DOM (backgrounded, paused) after the user backs out of a watch view to the
home screen, so pressing L2/R2 there called `pl.seekTo(...)`/`seekBy(...)` and the TVHTML5 player
*resumed* — the previous video played audibly behind the home screen. `config/scripts/chapter_seek.js`
and `skip.js` now bail early unless `location.hash.indexOf('/watch') >= 0` — the SAME "player open"
signal `player_state.js` already computes and `player.cpp` trusts to pick `Layer::Player` (kept as a
substring match, hash-only, so the seek guard and the layer poll stay byte-for-byte in lockstep rather
than tightening one in isolation). The guard lives in the JS, not `input.cpp`'s trigger dispatch,
because it must read the route at the *instant* of the press: `layer()` is a poll result up to
`poll_ms` stale, so a fast "back out → press L2" would beat a C++-only gate. The identical automatic
path — `player_play.js` via `on_resume()` (nudges play unconditionally after every suspend/resume,
`player.cpp` on_resume) — carried the same class of bug (resume on the home screen → backgrounded
video plays) and got the same guard. The voice-duck resume (`voice_play.js`) is NOT affected: `unduck()`
only fires when `ducked_` is set, which requires `voice_pause` to have paused a *playing* video, so it
is self-balancing. All three guards are hot-swappable script edits; no launcher rebuild is needed for
the logic. **Hardware-verified 2026-07-14 (OLED, Game Mode):** the fixed scripts were hot-swapped onto
the installed Flatpak via `DECKBACK_SCRIPTS_DIR` (journald confirmed all three `scripts: '…' overridden
from …` at startup), and L2/R2 on the home screen after backing out of playback no longer resumes the
backgrounded video — the reported bug is gone. The `player_play.js` suspend/resume guard shares the
identical route check but was not independently exercised via `just soak`; that path's `just soak`
scenario runs on `/watch` (unchanged), so no regression is expected.
