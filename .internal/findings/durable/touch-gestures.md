---
scope: durable
title: Touch gestures — the page-layer router, and why the EVIOCGRAB price is gone
created: 2026-07-31
status: implemented and TUNED ON HARDWARE 2026-07-31 (OLED); every gesture exercised by a real
        finger. Multitouch delivery is now PROVEN (§8). The delivery regression that looked like a
        gold-engine bug is RESOLVED — it was our own touch-mode guard skipping a same-value write
        (§7.0.2), fixed and proven before/after on a live process. Untested on LCD, no second unit.
supersedes: input-ux §11 (the "gestures force a permanent exclusive grab" cost model)
sources:
  - the 2026-07-31 touch probe (`just touch-probe --modes 4`), OLED, gamescope 3.16.23.4
  - the 2026-07-31 live before/after on the Deck: same process, same-value atom write, 0 -> 6
    sequences (§7.0.2) — the measurement that overturned the engine-preset theory
  - durable/touch-lock.md (the grab is not available to us at all)
  - input-ux §3 (mobile's spatial model), §4 (the never-silent policy), §7 (the gaps), §11 (priced)
---

# Touch gestures

## 1. What the probe changed

`input-ux §11` priced a gesture layer as an architectural commitment: `EVIOCGRAB` is exclusive, so
reading touch meant starving gamescope of it, which meant **we** would own tap-to-activate, palm
rejection, the 720p-letterbox coordinate transform, and a crash would kill the touchscreen until the
kernel released the grab. On that basis the recommendation was to treat the layer as a deliberate
commitment and probably not pay for it.

**That price does not exist.** Two findings removed it:

1. `touch-lock.md` (2026-07-10) — the launcher runs as the seat user `deck`, which cannot `open()`
   the FTS3528 node at all. There was never a grab to take.
2. The touch probe (2026-07-31) — with gamescope's `STEAM_TOUCH_CLICK_MODE` at **4 (passthrough)**,
   **real `wl_touch` reaches Blink**: 9 taps produced 9 `touchstart` + 9 `touchend` with per-finger
   identifiers and page-space coordinates, and left/right thirds separated cleanly
   (`left=12 middle=0 right=6` at viewport width 1279).

So the router is an ordinary capture-phase listener in the page — the same mechanism as
`no_pointer.js`, needing no privileges, leaving touch exactly as it found it if the launcher dies.
`input-ux §11` is superseded. So, since the same session, is §3's "don't rely on multitouch" — see
§4: it was Valve's FAQ, and the panel disagrees.

## 2. The architecture, and why it is split

```
finger -> gamescope (mode 4) -> Xwayland -> XI2 -> Blink
       -> config/scripts/touch_gestures.js   recognise + SWALLOW, queue the gesture
       -> launcher polls __deckbackGestures.drain() over CDP every touch_poll_ms
       -> launcher/src/gestures.cpp          dispatch a TRUSTED key / player call
```

Both halves of the split are forced, not stylistic:

* **The page must recognise.** Only it sees touch — the launcher cannot open the panel node.
* **The launcher must act.** A page cannot synthesize a trusted key (m114.md), and Leanback ignores
  untrusted ones.
* **Polling, not events.** `devtools.cpp` drops every CDP event; there is no demux (its own comment
  says so), so the page has no channel to push on. `touch_poll_ms` (50 ms) is therefore the entire
  touch input latency budget: recognition is immediate, delivery waits one tick.

The router **swallows what it recognises** (`stopImmediatePropagation` + `preventDefault`), so
Leanback never sees the finger and cannot double-act. That is the same trick the probe's recorder
used, which is why the probe was a live rehearsal of this design.

## 3. The invariant that makes it work or look broken

`touch_gestures` and `disable_touch` are **opposites and mutually exclusive**:

| | `disable_touch` | `touch_gestures` |
|---|---|---|
| page script | `no_pointer.js` swallows everything | `touch_gestures.js` recognises, then swallows |
| gamescope mode | 0 (hover) — motion only | 4 (passthrough) — real `wl_touch` |

Both on is not a merge, it is a **dead touchscreen**: `no_pointer.js` registers a document-start
capture listener calling `stopImmediatePropagation()`, so the router receives nothing and the panel
looks broken rather than misconfigured. `main.cpp` resolves this once, warns, and keeps the lock
(the safer of the two). This is exactly the failure the touch probe refuses to run into — it aborts
rather than read zero for the wrong reason.

## 4. Multitouch is inert — but delivery is now PROVEN

The design ships every multi-finger sequence **abandoned**: it emits nothing and increments
`stats().multiFinger`. That counter was built as a self-probe — "if it climbs during real use,
multitouch is being delivered and pinch work can start".

**It climbed.** After a few minutes of ordinary use on 2026-07-31: `multiFinger: 5` across
`sequences: 48`. The counter only increments when Blink reports **more than one simultaneous touch
point**, so gamescope → Xwayland → XI2 → Blink carries multitouch — on a device whose
`navigator.maxTouchPoints` reads **0**.

This retires the "**don't rely on multitouch**" caveat in `input-ux` §3, which was sourced from
Valve's Deck FAQ rather than from measurement.

**What is still unknown, and what a pinch design needs:** how MANY simultaneous points arrive. The
recogniser abandons a sequence on the *second* finger, so it has never counted past 2, and it does
not check whether coordinates stay sane with two fingers down. That is a short probe (raise the
abandon threshold, read `maxTouches`), and it must run before anything two-fingered is designed.

## 5. The gesture set (input-ux §3 + §7)

| Gesture | Action | Why |
|---|---|---|
| swipe / drag | **exactly one** move per gesture, fired as the finger passes `touch_step_px` (or on release) | Leanback has no pointer scroll; rails longer than a screen are untraversable by touch — the Switch YouTube app's most-criticised gap |
| fast flick | momentum arrows, capped — **default off** | a discrete-focus UI overshoots rather than glides |
| swipe from the left edge | `Escape` | there is no other touch Back path |
| double-tap left / right third | seek ∓`skip_seconds`, **accumulating** 10/20/30/40 on repeats | mobile's left-half/right-half model |
| single tap | `Enter` | activate tile / toggle chrome |
| hold left third | **0.5× slow motion** until released | a rate, not a rewind — see below |
| hold right third / centre | **2×** until released | mobile's hold-for-2×, unblocked by P12.0b |
| any seek or hold | an **on-screen indicator** on that side (`+30 s`, `0.5x`) | touch has no key to look at and no button to feel |

**Rewind is not a rate, which is why hold-left slows down instead.** Chromium has no negative
`playbackRate`: assigning one throws or is ignored, and there is no decoder path for it. A rewind
would therefore have been stepped seeks on a timer — a second mechanism with its own feel and its
own decoder thrash — to express something the advertised ladder already covers. Slow motion is a
real rate, so both directions are one mechanism differing only in the number, and `hold_scrub.js`
restores the rate the user was actually watching at rather than a hardcoded 1×.

### 5.1 Two designs a finger rejected, and why neither was a tuning problem

Both were found in one session against a real panel, and both are now regression fixtures.

**Proportional scrolling is wrong here, not mistuned.** The first design emitted one arrow per
`stepPx` of travel, live during the drag, so scrolling would track the finger. On hardware a normal
swipe **double-stepped**. The reason is categorical: Leanback moves *selection* one key at a time
and cannot interpolate, so "distance travelled" has no continuous meaning to it — there is nothing
for a proportional model to be proportional *to*.

**And no value of `stepPx` fixes that**, which is the part worth remembering. Raising it until a
normal swipe stops double-stepping is exactly the point at which a *short* flick travels less than
one step and emits **nothing at all** — a panel that feels dead. Both complaints arrived in that
order, from the same number being asked to do two jobs.

The resolution splits the jobs: **`maxSteps` (default 1) is the rule** — one gesture is one move,
at any distance or speed — and `stepPx` only decides *when within the gesture* it fires, so a long
drag still answers before the finger lifts. Any swipe past the tap slop is worth one move.
`touch_max_steps > 1` restores the proportional model for anyone who wants it.

**Direction convention:** content follows the finger. Dragging **up** moves **down** the list, so a
finger-up drag emits `ArrowDown`. The arrow names the list movement, never the finger movement.

A single tap waits out the double-tap window before committing, so a double-tap seek does not also
fire an `Enter`. That is the same latency mobile YouTube has, for the same reason.

## 6. Turning it off is a hardware button — and a rear grip, for free (§4 policy)

`input-ux §4` requires the touch state to be user-controllable from the controller and **never
silent** — the moment you want touch off is usually the moment a palm is already on the panel.
`y` → `toggle_gestures` ships bound (Y was vacated when voice search was removed), toasts the new
state, and rumbles. **This is also the R4 grip**: `config/steam_input.vdf` maps
`button_back_right_upper` to `xinput_button Y`, so the grip arrives at the launcher as the same
evdev code (308) and already toggles — which is the ergonomically right home for a modal toggle you
reach for while a palm is already on the panel. The launcher cannot *distinguish* them; making R4 do
something different from Y is P12.7 (repoint it to a distinct xinput button first). Two channels because either alone can be missed: a toast is invisible to someone
looking at their hand, a rumble is ambiguous on its own.

The binding ships bound even though `touch_gestures` ships **false**, so enabling is one config key
and not two; while off, the binding is inert and says so once at startup.

## 7. What is verified and what is not

**Verified (L0, off-Deck):** the recogniser, against synthetic touch streams — 21 cases in
`tests/js/touch_gestures.test.js` covering every gesture, the direction convention, axis lock, the
edge-swipe/scroll split, double-tap pairing windows, multi-finger inertness, queue bounding and the
swallow. The launcher half — mapping and drain decode — is `launcher/tests/gestures_test.cpp`,
including that an unreadable answer is not an empty queue.

**Verified ON HARDWARE (2026-07-31, OLED, gamescope 3.16.23.4), by a real finger:** the whole stack
came up (`touch mode held at passthrough`, router installed, connected to the page), and every
gesture was exercised — swipe (after two rounds of tuning, §5.1), single tap (activates the selected
video / pauses), double-tap on playback (seeks forward), press-and-hold at 2× **and hold-left at
0.5× slow motion** (so P12.0b's `playbackRate` finding pays off end to end), and the Y toggle (off
and on, with its toast). 50 ms polling was not reported as perceptible.

**Also verified by finger on the FIXED build (2026-07-31, OLED, after §7.0.2):** gestures on a
relaunch into an already-primed session — the exact condition that used to kill them — the
**left-edge swipe → Back** (previously untried by anyone, in any build), the **Y-during-hold**
release (2× returns to normal instead of stranding the rate), and **recovery across a Steam QAM
brightness change**, which is the failure the user originally reported.

**NOT verified:**

* the **seek indicator past six double-taps** — the last of the three §7.1 review fixes still
  carrying L0 coverage only. The other two (cursor hiding, Y-during-hold release) are now confirmed
  by finger;
* nothing on an **LCD unit** or any second device;
* whether **mode 4 stays put over a long session** — partly answered and worse than assumed: it does
  NOT stay put, Steam re-asserts 1 on every focus change (§7.0.2). What is untested is whether the
  750 ms heartbeat holds across hours, sleep/resume, and Steam updates;
* the feel numbers (`stepPx 70`, `tapSlop 16`) are one person's hands on one panel.

### 7.0.2 ★ RESOLVED — it was OUR guard, not the engine: the write we optimised away (2026-07-31)

**§7.0 and §7.0.1 below are kept for history and are WRONG about the cause.** They convicted the
`gold`/ThinLTO engine on circumstantial evidence. The engine is innocent. So is the cursor script,
so are all three review fixes, and so is the cold boot that an even earlier draft blamed.

**The cause is `launcher/src/touchmode.cpp`: the guard wrote the touch-mode atom only when its value
was wrong.**

```cpp
if (our_window_is_focused(c, root) && read_cardinal(c, root, mode_atom) != want) {   // the bug
```

gamescope re-points touch routing when it sees the **`PropertyNotify`**, not by reading the atom's
value when a window maps. A run that inherits the correct value from a *previous* run therefore
writes nothing, gamescope never learns our new window exists, and every finger falls on the floor —
while every diagnostic reads healthy, which is exactly why this cost a day. `STEAM_TOUCH_CLICK_MODE`
= 4, router installed, listeners provably attached, panel enumerated, focus ours: all true, all
useless. **The absence of a symptom in the environment was mistaken for the absence of a cause in
our code.**

X11 emits a `PropertyNotify` on *every* `xcb_change_property` call, whether or not the value
changed. So a same-value write is a real signal, and skipping it — which reads like an obvious
optimisation, and which is correct for any atom whose *value* is the whole message — is the defect.

**Proven, before and after, on one running process (OLED, 2026-07-31):**

| | `sequences` | `emitted` |
|---|---|---|
| gestures dead, mode already 4, guard idle | **0** | **0** |
| after `xprop -root -set STEAM_TOUCH_CLICK_MODE 4` — *the same value* | **6** | **6** |

Nothing else changed: same process, same bundle, same engine, same config, no relaunch. Touch came
back mid-session on a property write that altered nothing.

**Why it hid for so long, and why it looked like a release-only regression.** The bug needs the mode
to *already* be right at startup, which only happens on a second launch into a session a first
launch left behind. Every hardware confirmation of this feature ran after `just touch-probe`, which
cycles modes 4,1,0 and so always left a change for the guard to make. v0.0.9 was simply the first
build launched into an already-primed session — and it happened to be the first `gold` build too.
Two unrelated firsts coinciding produced a preset theory that survived a whole investigation.
**`out/release` was never tested against, which made it a plausible-looking suspect; being untested
is not evidence of being guilty.**

**There is a SECOND way in, and it is the one users actually hit: changing screen brightness.** The
user reported touch dying after a brightness change, which goes through the Steam QAM. Measured
on-Deck while it was broken:

```
:0  mode=1  focus=steam                    <- Steam's Xwayland
:1  mode=4  focus=chromium-content_shell   <- ours
```

Both are true at once, and the *effective* behaviour was mode 1: a finger moved the cursor and
highlighted tiles instead of delivering `wl_touch`. gamescope runs one Xwayland per display
(`--xwayland-count 2`), each with its own root and its own copy of the atom, but the touch mode it
enforces is **global** — and Steam writes its own value on `:0` when the overlay opens. So:

> **The value we can read is a stale mirror of a global state somebody else also writes.** It is not
> evidence about the effective mode, and comparing against it is not a valid guard at all — not an
> optimisation with an edge case, but a check on the wrong variable.

The tile-highlighting is itself the tell, and worth remembering: it is **cursor motion**, so it is
CSS `:hover`, which no amount of `stopImmediatePropagation` can swallow. **A page-layer swallow can
suppress handlers but never hover** — if tiles light up under a finger, pointer emulation is live
and the mode is wrong, whatever the atom says.

**A rising-edge write was tried first, and the hardware rejected it.** The idea was to write
unconditionally whenever our window *gains* focus — a fresh launch and the QAM handing focus back
being the same event. A 60-second recording during a real brightness change killed it:

```
focus on :1   chromium-content_shell   <- all 95 samples; NEVER changed
:1 mode       4                        <- never changed
:0 mode       1 -> 4 -> 1              <- moved, on Steam's display
```

**Our window never loses X input focus on `:1` when the QAM opens.** So there is no rising edge to
catch, and no drift to correct: *every variable the guard can observe stays constant while the
effective mode moves underneath it.* The guard is blind to this failure by construction.

**Which is why the fix is a heartbeat: assert every tick while focused, and never read first.** Once
you accept that we cannot *detect* the bad state, the only correct move is to continuously assert
the good one. This is not a workaround for an unknown mechanism — the mechanism is now measured, and
it says we are **contesting** the atom, not initialising it:

| `GAMESCOPE_FOCUSED_APP` | `:0` mode |
|---|---|
| `769` (Steam overlay / QAM) | **4** |
| our appid | **1** |

**Steam sets mode 1 whenever a game is focused**, on every focus change, because it assumes a game
wants click emulation. A guard that writes once always loses; a guard that writes only on a mismatch
never even tries.

**Measured on-Deck (2026-07-31, OLED), all four claims the design rests on:**

| Test | Result |
|---|---|
| same-value write revives a dead session | `sequences` 0 → **6** |
| same-value write revives after a brightness change | 0 → **28**, tiles stopped highlighting |
| 750 ms heartbeat, writes landing mid-swipe | **59** sequences, none disrupted |
| Steam QAM used by finger *during* the heartbeat | **fully usable** |
| heartbeat loop stops | touch breaks again within seconds — `:0` = 1, `:1` = 4 |

The QAM staying usable is not luck: Steam itself wants mode 4 while its overlay is focused, so we
agree with it exactly when it matters, and the focus gate keeps us quiet if the user leaves for
another app entirely.

**A better focus signal exists if one is ever needed.** `GAMESCOPE_FOCUSED_APP` on `:0`'s root is
gamescope's own authoritative notion of focus and *does* track the QAM (it flips to `769` and back),
where `XGetInputFocus` on `:1` does not move at all. We do not use it — the heartbeat needs no such
discrimination and fewer moving parts is worth more — but it is the tool to reach for if we ever
must know when Steam owns the screen. The decision is extracted as the
pure `should_write_mode(focused, was_focused, current, want)` so it carries L0 coverage;
`launcher/tests/touchmode_test.cpp` pins the case that reads like a pointless no-op — focused, value
already correct, first tick — **because that case is the entire fix**.

**What this says about the test suite, which is the durable lesson.** Nothing at any tier asserted
that touch ARRIVES; the L2 test asserts the mode is *eventually* 4, which was true throughout the
failure and would have stayed green. A check on the state we intend, rather than on the effect we
need, passes hardest exactly when the mechanism that turns one into the other is broken.

### 7.0 ✗ SUPERSEDED by §7.0.2 — the "engine preset" theory, kept for history

**This section's conclusion is wrong.** It is retained because the reasoning that produced it is
instructive, not because it is true. Read §7.0.2 first.

A real finger produces **zero events of any family** — not touch, not pointer, not mouse — on the
v0.0.9 build, while everything around it checks out: router present/enabled/configured, listeners
provably attached (a synthetic `touchstart` on `window` incremented `sequences` 0 → 1), cursor fix
applied, `STEAM_TOUCH_CLICK_MODE` = 4 stable on the display holding our focus, panel enumerated
(`FTS3528`, event14/15), no external display attached. Mode 4 does no pointer emulation, so silence
rather than stray mousemoves is the expected shape of "touch never arrived".

**An earlier version of this section blamed a cold boot. That was wrong, and the error is
instructive.** The reboot happened BEFORE the build that was then tested by hand and worked
(deployed 23:26, confirmed gesture by gesture including 0.5× slow motion); the failure appeared only
on the build deployed at 23:55. Ordering the events wrongly turned "it worked, then we changed
things, then it stopped" into "a reboot changed something", and sent the investigation at the
compositor instead of at the diff. **The user's own framing — it worked before the changes — was the
correct read and was talked past.**

**TWO things changed between the working build and v0.0.9, not one:**

1. **Three review fixes** (§7.1). Only one touches the page: the cursor hiding.
2. **The entire engine binary.** `just release` builds `gold` + ThinLTO into `out/release` and
   packages `preset=release`; every build ever tested by hand was `deck`/qa out of `out/deck`. **The
   engine in v0.0.9 has never been exercised by a finger, or by anything else beyond `just smoke`.**

**Weighing them.** The script change is a poor fit for the symptom: `install()` runs BEFORE
`hideCursor()`, the listeners were observed attached on the broken build, and `cursor: none` is
paint state with no path to XI2/`wl_touch` delivery. It is not exonerated by `no_pointer.js` having
carried the same code for months either — in that mode touch is *meant* to be dead, so a suppression
there would never have been noticed. The launcher-side fixes cannot be responsible at all: one is
arithmetic behind the indicator text, the other only runs on `setEnabled(false)`.

That leaves the untested `gold` engine as the more plausible candidate — and if it IS the engine,
the finding is much larger than touch: it means the preset we ship is not the preset we validate.

**Mitigation applied 2026-07-31, before the cause is known.** The router is restored
**byte-identical** to the build that was verified by hand, and the cursor hiding moved OUT of it into
its own script (`config/scripts/hide_cursor.js`) behind its own flag (`touch_hide_cursor`, default
on, user-overridable). The two suspects were entangled in one file, so neither could be dropped
without dropping the other; now `"touch_hide_cursor": false` in `user.json` removes exactly the
cursor script and nothing else, with no rebuild. An L0 test pins that the cursor script registers
**no input listeners at all**, so its ability to affect touch is bounded by construction rather than
by argument.

This does not answer the question — it makes the question cheap to answer on hardware, and it means
the shipped router is the code that was actually tested.

### 7.0.1 The page scripts are EXONERATED — off-Deck bisect, 2026-07-31

**Its verdict holds; its inference does not.** The bisect correctly cleared the page scripts, and the
categorical argument in it is sound. But "what remains is the engine" was a false dichotomy: it
enumerated the suspects it could *see* — page scripts and the engine binary — and never listed the
launcher's own touch-mode guard, which is neither. §7.0.2 has the cause. The lesson is that
narrowing by elimination is only as good as the suspect list, and ours omitted our own code.

`just touch-bisect` runs the 2×2 in the build container: each engine preset × {router,
router+hide_cursor}, dispatching **real** touch through CDP `Input.dispatchTouchEvent`.

| Engine | Scripts | Result |
|---|---|---|
| `deck` (every hand-tested build) | router | TOUCH ARRIVES |
| `deck` | router + hide_cursor | TOUCH ARRIVES |
| `release` (gold+ThinLTO, ships in v0.0.9) | router | TOUCH ARRIVES |
| `release` | router + hide_cursor | TOUCH ARRIVES |

`maxTouchPoints` reads 0 in all four, as it does on-Deck, and touch arrives anyway — the same
capability-query lie recorded in §8.4 of feature-landscape.

**And a logical step makes this stronger than four data points: a page script cannot reach X11/XI2.**
It can only act on events already delivered to Blink. On-Deck *nothing* was delivered — not touch,
not pointer, not mouse — so no page script can be the cause, categorically. The two launcher-side
fixes are excluded the same way: one is arithmetic behind the indicator's text, the other only runs
on `setEnabled(false)`.

**So the §7.0 mitigation (splitting the cursor into its own script) was aimed at the wrong suspect.**
It is kept because a runtime switch and a bounded, separately-tested script are worth having, not
because it fixes anything.

**What remains** is the layer the bisect deliberately cannot reach — gamescope → Xwayland → XI2 →
Blink — where the engine binary is the only thing that changed between the working build and
v0.0.9. `flatpak.sh` stages `out/<preset>`, so the deck bundles shipped `out/deck` and v0.0.9 ships
`out/release`; they are different binaries. Blink-level touch handling is identical between them
(rows 3–4 above), so the question is specifically whether the gold build enumerates or receives from
an XI2 touch device differently — which CDP injection bypasses and Xvfb cannot reproduce, because it
has no XI2 input devices to advertise.

**Next decisive experiment (needs a Deck and a finger):** build a `deck`-preset bundle from current
`main` and deploy it. Gestures working ⇒ the engine preset is implicated and no release may ship from
`gold` until that is understood. Gestures still dead ⇒ neither the code nor the preset, and the cause
is in the device's session/compositor state.

**The armed A/B on the Deck is now redundant** — the bisect answered its question more cleanly.

 The v0.0.9 app on the Deck is running the
EXACT pre-review script (hot-pushed over CDP, `cursor` empty again) with per-family counters reset.
Tap, then read `window.__dbFam` and `stats()`:

* **events appear** → the cursor change is the cause, the engine is exonerated, fix is in the script;
* **still zero** → the script is exonerated and the `gold`/ThinLTO engine is the difference, which
  needs its own investigation before any release ships from that preset.

**Why every test missed this.** No test covers "touch actually ARRIVES" — they cover what happens
once it has. The L2 test added the same day
(`test_gamescope_touch_mode_matches_the_configured_policy`) passes throughout: it asserts the mode is
eventually 4, which was always true. And nothing at any tier runs against the `gold` build, so a
release-only regression has no gate in front of it at all.

**Blast radius:** `touch_gestures` defaults to **false**, so a normal 0.0.9 install is unaffected —
the panel stays inert exactly as in 0.0.8. Only opt-in users would meet a dead feature.

### 7.1 Three bugs a read-through found that the tests and the finger both missed

Caught reviewing the finished feature, after it had been driven by hand on-Deck. All three would
have shipped in v0.0.9; each is now a regression test.

**The cursor came back.** `no_pointer.js` hides the X cursor (`cursor: none`, via a constructable
stylesheet because the CSP drops inline `<style>`). The two touch policies are mutually exclusive,
so with the router installed that script is *not* — and gamescope composites our X cursor, leaving a
pointer visible over a 10-foot UI. Nothing in the gesture path hid it. **Inherited behaviour is not
inherited when you replace the thing it lived in**, and mutual exclusion is exactly where that bites.

**...and the first fix silently did nothing.** It used a bare `document` where the rest of the file
uses `W.document`, inside a `try/catch` that swallowed the reference error. It read correctly, ran,
and hid nothing. The only reason it surfaced is that the test asserts the *effect* (the property is
set, the sheet is adopted) rather than that the function was reached — a test of the call would have
passed.

**A cap that capped nothing, and an indicator that lied.** The accumulating seek multiplied
`skip_seconds` by the run length for the *indicator* while always seeking a constant increment, and
then clamped the multiplier at 6 with the comment "so a long run cannot cross a whole video". The
seek never used the multiplier, so the clamp protected nothing — it only truncated the display, so
from the seventh double-tap the indicator understated where the user actually was. A comment
describing behaviour the code does not have is worse than no comment: it stops the next reader from
looking.

**Turning the router off stranded the playback rate.** `setEnabled(false)` discards the queue, so a
hold in progress never delivered its release and the video stayed at 2× (or 0.5×) permanently —
pressing Y to stop touch doing things left the most visible thing touch had done. Every individual
piece was correct; the bug lived in the interaction between the toggle and the queue, which is the
kind only a state walk-through finds. The launcher now stops the scrub unconditionally when it
disables, rather than trusting a queue it just threw away.

**Default OFF**, and it stays off until at least a second unit agrees. `navigator.maxTouchPoints`
reads **0** even where touch demonstrably works, so nothing can feature-detect this — any code that
gates on `maxTouchPoints` or `'ontouchstart' in window` would disable touch on a device where it
works. That trap is worth remembering: it is the kind of check a gesture layer naturally writes.
