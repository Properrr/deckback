---
scope: durable
title: Touch gestures — the page-layer router
created: 2026-07-31
status: implemented and tuned on hardware 2026-07-31 (OLED, gamescope 3.16.23.4); every gesture
        exercised by a real finger; multitouch delivery proven (§4). Default OFF. Untested on LCD,
        no second unit.
supersedes: input-ux §11 (the "gestures force a permanent exclusive grab" cost model)
sources:
  - the touch probe (`just touch-probe --modes 4`), OLED, gamescope 3.16.23.4, 2026-07-31
  - the on-Deck same-value atom write, same process, 0 -> 6 sequences (§7.0.2), 2026-07-31
  - durable/touch-lock.md (the grab is not available to us at all)
  - input-ux §3 (mobile's spatial model), §4 (the never-silent policy), §7 (the gaps)
---

# Touch gestures

## 1. Why a page-layer router is possible

A gesture layer does not need `EVIOCGRAB`, so `input-ux §11`'s price (we would own tap-to-activate,
palm rejection and the letterbox transform, and a crash would kill the touchscreen) does not apply:

1. The launcher cannot `open()` the FTS3528 node at all (`touch-lock.md`), so there is no grab to take.
2. With gamescope's `STEAM_TOUCH_CLICK_MODE` at **4 (passthrough)**, **real `wl_touch` reaches
   Blink**: 9 taps produced 9 `touchstart` + 9 `touchend` with per-finger identifiers and page-space
   coordinates, and left/right thirds separated cleanly (`left=12 middle=0 right=6` at viewport
   width 1279).

So the router is an ordinary capture-phase listener in the page — the same mechanism as
`no_pointer.js` — needing no privileges and leaving touch as it found it if the launcher dies.

## 2. The architecture, and why it is split

```
finger -> gamescope (mode 4) -> Xwayland -> XI2 -> Blink
       -> config/scripts/touch_gestures.js   recognise + SWALLOW, queue the gesture
       -> launcher polls __deckbackGestures.drain() over CDP every touch_poll_ms
       -> launcher/src/gestures.cpp          dispatch a TRUSTED key / player call
```

* **The page must recognise.** Only it sees touch — the launcher cannot open the panel node.
* **The launcher must act.** A page cannot synthesize a trusted key, and Leanback ignores
  untrusted ones.
* **Polling, not events.** `devtools.cpp` drops every CDP event (no demux), so the page has no
  channel to push on. `touch_poll_ms` (50 ms) is the whole touch latency budget: recognition is
  immediate, delivery waits one tick. 50 ms was not perceptible on-Deck.

The router **swallows what it recognises** (`stopImmediatePropagation` + `preventDefault`), so
Leanback never sees the finger and cannot double-act.

## 3. `touch_gestures` and `disable_touch` are mutually exclusive

| | `disable_touch` (default on) | `touch_gestures` (default off) |
|---|---|---|
| page script | `no_pointer.js` swallows everything | `touch_gestures.js` recognises, then swallows |
| gamescope mode | 0 (hover) — motion only | 4 (passthrough) — real `wl_touch` |

Both on would be a **dead touchscreen**: `no_pointer.js`'s document-start capture listener calls
`stopImmediatePropagation()`, so the router receives nothing. `main.cpp` resolves it once, warns,
and keeps `disable_touch` (the safer of the two).

## 4. Multitouch is delivered, and deliberately inert

Every multi-finger sequence is **abandoned**: it emits nothing and increments
`stats().multiFinger`. That counter is a self-probe, and it climbed in ordinary use on-Deck:
`multiFinger: 5` across `sequences: 48`. It only increments when Blink reports **more than one
simultaneous touch point**, so gamescope → Xwayland → XI2 → Blink carries multitouch — on a device
whose `navigator.maxTouchPoints` reads **0**. This retires `input-ux` §3's "**don't rely on
multitouch**", which came from Valve's Deck FAQ rather than measurement.

**Still unknown, and required before any pinch design:** how MANY simultaneous points arrive, and
whether coordinates stay sane with two fingers down. The recogniser abandons on the second finger,
so it has never counted past 2. The probe: raise the abandon threshold and read `maxTouches`.

**Nothing can feature-detect touch here.** `navigator.maxTouchPoints` is 0 where touch works, so any
gate on `maxTouchPoints` / `'ontouchstart' in window` / `pointer: coarse` would disable touch exactly
where it works. That is why the feature is config-gated and default OFF until a second unit agrees.

## 5. The gesture set (input-ux §3 + §7)

| Gesture | Action | Why |
|---|---|---|
| swipe / drag | **exactly one** move per gesture, fired as the finger passes `touch_step_px` (70) or on release | Leanback has no pointer scroll; rails longer than a screen are otherwise untraversable by touch |
| fast flick | momentum arrows, capped — **default off** | a discrete-focus UI overshoots rather than glides |
| swipe from the left edge (`touch_edge_px` 40) | `Escape` | there is no other touch Back path |
| double-tap left / right third | seek ∓`skip_seconds`, **accumulating** 10/20/30/40 on repeats | mobile's left-half/right-half model |
| single tap | `Enter` | activate tile / toggle chrome |
| hold left third (`touch_long_press_ms` 550) | **0.5× slow motion** until released | a rate, not a rewind — see below |
| hold right third / centre | **2×** until released | mobile's hold-for-2× |
| any seek or hold | an **on-screen indicator** on that side (`+30 s`, `0.5x`; `gesture_hud.js`) | touch has no key to look at and no button to feel |

**Hold-left slows down because Chromium has no negative `playbackRate`** — a rewind would need
stepped seeks on a timer, a second mechanism with its own decoder thrash. Both holds are real rates
on the ladder the player advertises, snapped to the nearest value, and `hold_scrub.js` restores the
rate the user was watching at rather than 1×.

A single tap waits out the double-tap window before committing, so a double-tap seek does not also
fire an `Enter` (the same latency mobile YouTube has).

**Direction convention:** content follows the finger. A finger-up drag moves **down** the list and
emits `ArrowDown`; the arrow names the list movement, never the finger movement.

### 5.1 One gesture is one move — proportional scrolling is wrong here

Leanback moves *selection* one key at a time and cannot interpolate, so "distance travelled" has no
continuous meaning to it. Emitting one arrow per `stepPx` of travel double-stepped on a normal swipe
on hardware, and **no value of `stepPx` fixes that**: the value large enough to stop a normal swipe
double-stepping is the value at which a short flick emits nothing. So **`touch_max_steps` (default 1)
is the rule** — one gesture is one move at any distance or speed — and `touch_step_px` only decides
*when within the gesture* the move fires, so a long drag answers before the finger lifts. Any swipe
past the tap slop is worth one move. `touch_max_steps > 1` restores proportional scrolling. Both
behaviours are regression fixtures.

## 6. Turning it off is a hardware button — and the R4 grip (input-ux §4 policy)

`input-ux §4` requires the touch state to be controller-controllable and **never silent** — the
moment you want touch off is usually the moment a palm is on the panel. `y` → `toggle_gestures`
ships bound, toasts the new state, and rumbles (a toast is invisible to someone looking at their
hand; a rumble alone is ambiguous). **It is also the R4 grip:** `config/steam_input.vdf` maps
`button_back_right_upper` to `xinput_button Y`, which reaches the launcher as the same evdev code
(308). The launcher cannot distinguish them; giving R4 its own action is P12.7 (repoint it to a
distinct xinput button first).

The binding ships bound even though `touch_gestures` ships false, so enabling is one config key; while
off, the binding is inert and says so once at startup.

## 7. What is verified and what is not

**Verified (L0, off-Deck):** the recogniser against synthetic touch streams in
`tests/js/touch_gestures.test.js` (every gesture, direction convention, axis lock, edge-swipe/scroll
split, double-tap pairing windows, multi-finger inertness, queue bounding, the swallow); the launcher
half (mapping, drain decode, an unreadable answer is not an empty queue) in
`launcher/tests/gestures_test.cpp`.

**Verified ON HARDWARE by a real finger (2026-07-31, OLED, gamescope 3.16.23.4):** the stack comes up
(`touch mode held at passthrough`, router installed and connected), swipe, single tap (activates the
selected video / pauses), double-tap seek, hold at 2× and hold-left at 0.5×, the Y toggle (off and
on, with its toast), the left-edge swipe → Back, Y-during-hold releasing the rate, gestures on a
relaunch into an already-primed session, recovery across a Steam QAM brightness change, and the
hidden cursor.

**NOT verified:**

* the seek indicator past six double-taps (L0 only);
* anything on an **LCD unit** or a second device;
* whether the 750 ms touch-mode heartbeat (§7.0.2) holds across hours, sleep/resume and Steam
  updates;
* the feel numbers (`stepPx 70`, `tapSlop 16`) are one person's hands on one panel.

There is still no test at any tier that asserts touch **arrives** on-Deck; the L2 test
(`test_gamescope_touch_mode_matches_the_configured_policy`) asserts the mode, which stays correct
while delivery is broken (§7.0.2).

### 7.0 The cursor script is separate (`touch_hide_cursor`)

gamescope composites our X cursor, and `no_pointer.js` (which hides it under `disable_touch`) is not
installed under gestures. The gesture policy therefore injects its own `config/scripts/hide_cursor.js`
behind `touch_hide_cursor` (default on, user-overridable in `user.json`). It is a separate script and
flag so that `"touch_hide_cursor": false` removes exactly the cursor rule and nothing else, with no
rebuild; an L0 test pins that it registers **no input listeners at all**, so it cannot affect touch
delivery by construction. The router itself is byte-identical to the build verified by finger.

### 7.0.1 Page scripts cannot cause a touch-delivery failure

A page script acts only on events already delivered to Blink; it cannot reach X11/XI2. If a real
finger produces **no events of any family** (touch, pointer, mouse), the cause is below the page.
`just touch-bisect` runs each engine preset × {router, router+hide_cursor} in the build container
with real CDP `Input.dispatchTouchEvent`; it bypasses gamescope/XI2, so only a FAILURE is conclusive.
`maxTouchPoints` reads 0 there too.

### 7.0.2 ★ The touch mode must be asserted every tick, never compared first

**gamescope re-routes touch when it sees a `PropertyNotify` on `STEAM_TOUCH_CLICK_MODE`, not by
reading the atom's value when a window maps.** X11 emits a `PropertyNotify` on every
`xcb_change_property`, even when the value is unchanged, so a same-value write is a real signal.
A guard that writes only when the value is wrong (`read != want`) therefore fails silently whenever
the value is already right — e.g. a relaunch into a session a previous run left at 4: every
diagnostic reads healthy (mode 4, router installed, listeners attached, focus ours) and every finger
falls on the floor.

**Proven on one running process (OLED, 2026-07-31):**

| | `sequences` | `emitted` |
|---|---|---|
| gestures dead, mode already 4, guard idle | **0** | **0** |
| after `xprop -root -set STEAM_TOUCH_CLICK_MODE 4` — *the same value* | **6** | **6** |

**The readable value is a stale mirror of a global state Steam also writes.** gamescope runs one
Xwayland per display (`--xwayland-count 2`), each root with its own copy of the atom, but the mode it
enforces is **global**, and **Steam sets mode 1 whenever a game is focused**, on every focus change:

| `GAMESCOPE_FOCUSED_APP` | `:0` mode |
|---|---|
| `769` (Steam overlay / QAM) | **4** |
| our appid | **1** |

During a QAM brightness change, `:1` (ours) read mode 4 with focus `chromium-content_shell`
throughout, while `:0` went 1 → 4 → 1 and the effective behaviour was mode 1. **Our window never loses
X input focus on `:1` when the QAM opens**, so neither a value check nor a focus rising edge can see
the failure. Hence the design: **write every tick (750 ms) while our window is focused, never read
first** — `should_write_mode(focused)` decides on focus alone, and
`launcher/tests/touchmode_test.cpp` pins that signature.

**Measured on-Deck (2026-07-31, OLED):**

| Test | Result |
|---|---|
| same-value write revives a dead session | `sequences` 0 → **6** |
| same-value write revives after a brightness change | 0 → **28**, tiles stopped highlighting |
| 750 ms heartbeat, writes landing mid-swipe | **59** sequences, none disrupted |
| Steam QAM used by finger *during* the heartbeat | **fully usable** |
| heartbeat loop stops | touch breaks again within seconds — `:0` = 1, `:1` = 4 |

The QAM stays usable because Steam itself wants mode 4 while its overlay is focused; the focus gate
keeps us quiet if the user leaves for another app.

**The tell for a wrong mode:** tiles highlighting under a finger is cursor motion — CSS `:hover` —
which no `stopImmediatePropagation` can swallow. A page-layer swallow suppresses handlers, never
hover; if tiles light up, pointer emulation is live whatever the atom says.

**A better focus signal, if ever needed:** `GAMESCOPE_FOCUSED_APP` on `:0`'s root is gamescope's
authoritative focus and does track the QAM (flips to `769` and back), where `XGetInputFocus` on `:1`
does not move. Unused — the heartbeat needs no such discrimination.

### 7.1 Behaviours pinned by regression tests

* **The cursor stays hidden under gestures** (§7.0). The test asserts the *effect* (property set,
  sheet adopted), not that the function was reached.
* **The accumulating seek indicator.** Each double-tap seeks a constant `skip_seconds`; the indicator
  shows the burst's running total (10, 20, 30, …) with no clamp, so it never understates the position.
* **Disabling the router stops any hold in progress.** `setEnabled(false)` discards the queue, so the
  launcher stops the scrub unconditionally rather than waiting for a release that will never be
  delivered.
