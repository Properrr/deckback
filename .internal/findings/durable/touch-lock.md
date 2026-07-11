---
scope: durable
verified: 2026-07-10
status: confirmed
sources:
  - launcher/src/touch.cpp
  - /usr/lib/udev/rules.d/70-uaccess.rules (on-Deck)
  - tests/deck/test_touch.py
  - tests/deck/test_capabilities.py
  - on-Deck fd scan + INPUT_PROP_DIRECT probe, 2026-07-10
---

# The touch lock cannot open the touchscreen, and the harness cannot fake one

**Verified on-Deck 2026-07-10 (OLED / Galileo), SteamOS `holo`, kernel valve24.4.** This settles
`.internal/TEST-PLAN.md` §2.C's single highest-risk item — "does `EVIOCGRAB` actually starve
gamescope of touch" — but not the way the question was posed. The grab never gets that far. The
launcher cannot even `open()` the panel, and the reason is OS policy, not our code, so it will not
change on a Cobalt bump. This is why the finding is durable, not milestone.

## What the feature assumed

`launcher/src/touch.cpp` (`TouchGuard`) opens `/dev/input/eventN` for the FTS3528 touchscreen and
holds `EVIOCGRAB` on it while the lock is engaged. The whole design rests on two unstated premises:
that the launcher can open the node, and that gamescope reads the *same* node so that grabbing it
starves the compositor. Both are false on SteamOS.

## Premise 1 — the launcher cannot open the touchscreen node (EACCES)

The panel is present exactly where the code looks for it:

    "FTS3528:00 2808:1015"  ->  event5 mouse0        # id matches kVendorFocaltech/kProductFts3528
    "FTS3528:00 2808:1015 UNKNOWN" -> event7 mouse2

But the app runs as the seat user `deck`, and:

    crw-rw---- root input  /dev/input/event5     # touchscreen — group 'input'
    crw-rw----+ root input /dev/input/event10    # gamepad — note the ACL '+'

`getfacl` shows the gamepad carries `user:deck:rw-`; the touchscreen carries **no** such entry, and
`input` contains only `brltty`. The difference is udev's `uaccess` tag: `70-uaccess.rules` grants an
ACL to the active-seat user for **joysticks only**
(`ENV{ID_INPUT_JOYSTICK}=="?*", TAG+="uaccess"`). Keyboards, mice, and **touchscreens deliberately
get none** — that is the standard defence against a seat program keylogging the console. So a direct
`open("/dev/input/event5", O_RDONLY)` as `deck` returns **EACCES**, `resolve_and_open()` hits its
`continue`, and `TouchGuard` reports `no touchscreen node found` — a node that is plainly there.

**The Flatpak does not help.** The shipped manifest grants `--device=input` (`devices=dri;input;`,
verified on the installed app). Inside the sandbox the nodes are all *visible* (`ls /dev/input`
shows event5) but `open()` still returns EACCES: `--device=input` bind-mounts the device nodes into
the sandbox, it does not change their owner/group or grant an ACL. The permission check is the
kernel's, and the kernel sees the same `root:input 0660` with `deck` not in `input`.

## Premise 2 — the harness cannot synthesize a panel gamescope will route

Even setting the launcher aside, the L2 positive control fails:
`test_touch.py::test_synthetic_tap_reaches_the_page_when_unlocked` creates a virtual multitouch
device via uinput and taps it, expecting a page click; **zero** clicks arrive
(`window.__deckbackClicks === 0`). gamescope's touch→pointer emulation does not pick up a
uinput-created panel, so `test_grab_starves_the_compositor_of_touch` cannot even establish its
baseline and skips ("could not find the panel we just created"). The one decisive test in the suite
has no way to run on this compositor as written.

Two things ruled out by experiment on 2026-07-10, so the next person does not re-run them:

- **It is not the missing `INPUT_PROP_DIRECT`.** The harness builds the panel with EV/KEY/ABS bits
  but never `UI_SET_PROPBIT(INPUT_PROP_DIRECT)`, so libinput would classify it as a touchpad, not a
  touchscreen — a plausible cause. A standalone probe that adds the prop bit and taps the centre
  three times still produced **zero** page clicks *and* zero `pointerdown`s. So a correctly-tagged
  synthetic touchscreen is ignored too; the prop bit is worth fixing in the harness for honesty but
  it is not the blocker.

## Premise 3 — nothing reads the touchscreen node, so a grab has nothing to starve

The deepest problem, and the one that would defeat even a launcher granted access. An fd scan of
every process (2026-07-10) shows **no process holds the touchscreen at all**, on either interface:

- evdev `event5`/`event7`: held by nobody. (The only evdev holders on the whole system are our own
  launcher on the gamepad `event10`, and `steamos-powerbuttond` on `event1`/`event3`.)
- hidraw: the FTS3528 is `hidraw1` (`HID_ID=0018:00002808:00001015`), held by nobody. Steam holds
  `hidraw2`, a *controller*, not the panel.

`gamescope-wl` (pid 1557) holds **zero** input-device fds. So whatever delivers a real finger-touch
to the compositor, it is not a persistent evdev reader of `event5` — and `EVIOCGRAB` only excludes
readers of the node it grabs.

## Premise 3, CONFIRMED by physical test — the grab does not block touch (2026-07-10)

Run end to end on the OLED with a human at the screen, the app foregrounded through Steam Game Mode:

- **Grant** (bypassing P1 for the test): `setfacl -m u:deck:rw /dev/input/event5 /dev/input/event7`,
  after which `deck` opens both nodes `O_RDWR`.
- **Baseline, no grab:** a CDP recorder counting `click`/`pointerdown` on the page; the user taps ~6×.
  Result: **4 clicks, 7 pointerdowns**, and a tap opened a video. Real touch reaches the app.
- **With `EVIOCGRAB` held on BOTH event5 and event7** by a standalone `deck` helper (grab returned
  success — nobody else held it): the user taps ~6× again. Result: **12 clicks, 14 pointerdowns**,
  and the page navigated. **Touch sailed straight through a successful exclusive grab.**

A successful `EVIOCGRAB` makes every other reader of that node go silent. Touch still flowed, so the
compositor is **not reading touch from event5/event7 at all**. The user's own words settle the path:
"the touchscreen just controls the mouse cursor" — gamescope consumes the panel and re-emits it as
*pointer* input (hence `pointerdown`/`click` and never `touchstart`), through a channel evdev
`EVIOCGRAB` cannot reach (libinput opening its own handle, or a hidraw/Steam-Input path — the FTS3528
also exposes `hidraw1`, and evdev grabs do not touch hidraw).

## Consequence

Touch lock is **non-functional as shipped, proven on hardware** — not three suspicions but one
demonstrated fact: `EVIOCGRAB` on the FTS3528 evdev nodes does not block touch, because the
compositor does not read touch there. This is on top of P1 (the launcher can't even open the node
without a udev/ACL grant we do not ship) and P2 (a synthetic panel can't reproduce it headlessly).

`touch.cpp`'s whole mechanism is therefore wrong for this platform, and no patch to it can work. The
only viable directions:

- Block touch at the **gamescope/Wayland layer** — if gamescope exposes a way to suppress its
  touch→pointer emulation (a touch mode, a runtime toggle) — since that is where the panel is
  actually consumed. This is research, not a `touch.cpp` fix.
- Or **remove the feature and its UI** so the app never advertises a lock that cannot engage.

Do not reach for "grant `deck` the node and ship a udev rule": the physical test already granted it
and the grab still did nothing. P1 was never the real blocker; P3 is, and it is fatal to the design.

Any real fix flows from that result: grant access and ship (if touch stops), or abandon evdev
grabbing for a gamescope/Wayland-layer block, or accept it cannot ship on stock SteamOS and remove
its UI so it never advertises a lock that did not engage (the failure mode `overlay.cpp`'s toast was
written to prevent, now caused one layer down).

Until then, `config/app.json`'s `touch_lock_enabled:true` promises something the OS forbids. The
launcher already logs `touch: no touchscreen node found` at startup on every Deck — that WARN is the
symptom, and it fires 100% of the time, not as drift.

Related: the CDP-injected toast that announces the lock (`launcher/src/overlay.cpp`) works fine
(Trusted Types notwithstanding, milestones/m114.md) — it is only the lock underneath it that does
not. The `docs/SUPPORT.md` touch-lock section and `config/app.json`'s touch keys should not describe
a working feature until this is resolved.

## RESOLVED 2026-07-10 — `disable_touch` makes touch inert at two layers that DO work

The user's actual goal was never "lock on a chord" — it was "touch shouldn't create a bad
experience." A finger under gamescope arrives as synthetic **mouse** events (a cursor that moves and
taps that click, navigating YouTube by accident). Two layers kill that, and both were verified:

- **Option A — page-level swallow (`config/no_pointer.js`, injected by the Navigator when
  `disable_touch`).** Capturing listeners on `window`/`document` at document-start
  `stopImmediatePropagation()` + `preventDefault()` every pointer/mouse/touch event before Leanback
  sees it. Leanback is D-pad/key driven and our gamepad input is KEY events, so nothing is lost.
  This alone makes taps inert regardless of what gamescope does; robust across focus changes; no new
  dependency.
- **Option B — gamescope hover (`launcher/src/touchmode.cpp`, `TouchModeGuard`).** gamescope's touch
  click mode is the **global, Steam-managed root atom `STEAM_TOUCH_CLICK_MODE`** — there is **no
  per-window override** in gamescope 3.16 (confirmed: the binary only interns that one atom). Mode
  **0 = hover** = cursor moves, no click. The guard polls the input-focused window (xcb,
  `XGetInputFocus` + WM_CLASS walk) and holds the atom at 0 **only while OUR content_shell window is
  focused** — never while a Steam overlay/QAM has focus, or it would kill touch there too. Best-effort:
  no libxcb / no DISPLAY / no atom ⇒ it logs once and does nothing, and Option A still covers.

**Measured on-Deck 2026-07-10 (OLED, Steam Game Mode, user at the panel):** with mode 0 set, the user
tapped and dragged the screen and the page counter showed **45 mousemoves, 0 clicks / 0 pointerdowns /
0 touchstarts**, and nothing navigated. So touch reached the app (mousemove ≠ 0, a real positive
control) yet produced no action. That single frame is both halves of the proof.

**Which X display, and why it matters (non-obvious).** The SteamOS gamescope session runs
`--xwayland-count 2`: **Steam lives on `:0`, a Steam-launched game on `:1`.** Our content_shell — and
the `STEAM_TOUCH_CLICK_MODE` atom the guard manages — are on **`:1`** when launched from Steam. The
guard connects via the launcher's own `DISPLAY` (`:1` under Steam), so it reads/writes the right
root; but ANY out-of-band check on `:0` sees only Steam's window ("steam" focused, mode 1) and will
wrongly conclude B never engaged. Verify B on the display where `xdotool getwindowfocus
getwindowclassname` returns `chromium-content_shell` (`tests/deck/test_touch.py` scans `:0/:1/:2`).
Confirmed on-Deck: on `:1`, focus = `chromium-content_shell` and `STEAM_TOUCH_CLICK_MODE = 0` held
steady. A useful consequence: because our app is on `:1` and the Steam overlay/QAM is on `:0`, B on
`:1` **cannot** interfere with overlay touch — they are different X servers, so the focus-gating is
belt-and-suspenders, not the thing that protects the QAM.

(An SSH launch that forces `DISPLAY=:0`, as the bring-up `relaunch.sh` does, puts the guard on the
wrong display, so B looks idle there even though A still works over CDP — a harness artifact, not a
product bug.)

**The cursor is hidden too (2026-07-10).** gamescope composites the *client's* X cursor, so
`cursor: none` on the page makes it draw nothing — verified on-Deck, the cursor vanishes on touch.
`no_pointer.js` sets it via a **constructable stylesheet** (`adoptedStyleSheets`), because a `<style>`
tag is blocked by youtube.com/tv's CSP `style-src` and a `*{cursor:none}` rule is needed to beat
per-element cursors (a button's `cursor:pointer`). So with `disable_touch` on there is no visible
cursor at all — not merely an auto-hiding one. (gamescope still offers no "ignore touch entirely" mode;
`-noTouchPointerEmulation` is unsupported on Xwayland. But it no longer matters: touch generates no
click and shows no cursor.)

The dead EVIOCGRAB lock now ships **off** (`touch_lock_enabled=false`), so the onboarding card no
longer advertises it. The `touch.cpp`/`TouchGuard`/chord code remains but dormant; removing it is a
separate cleanup. Tests: `launcher/tests/touchmode_test.cpp` (focus matcher + embedded-script
shape), `tests/deck/test_touch.py::test_disable_touch_swallows_pointer_events` (A, self-validating),
`::test_gamescope_touch_mode_is_hover_while_focused` (B, skips when unfocused).
