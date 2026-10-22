---
scope: durable
verified: 2026-07-10
status: confirmed
sources:
  - /usr/lib/udev/rules.d/70-uaccess.rules (on-Deck)
  - on-Deck fd scan, INPUT_PROP_DIRECT probe and physical grab test, 2026-07-10 (OLED)
  - launcher/src/touchmode.cpp, config/scripts/no_pointer.js
  - tests/deck/test_touch.py, launcher/tests/touchmode_test.cpp
---

# The EVIOCGRAB touch lock is dead; `disable_touch` replaces it

**Verified on-Deck 2026-07-10 (OLED / Galileo), SteamOS `holo`, kernel valve24.4.** An evdev
`EVIOCGRAB` on the FTS3528 touchscreen cannot lock touch on SteamOS, for reasons of OS policy and
compositor design rather than our code, so it will not change on a Cobalt bump. The lock code
(`touch.cpp`/`TouchGuard`/the chord) has been removed; the old `touch_lock_*` / `block_touchscreen`
keys are still accepted silently so older `app.json` files do not warn.

## Fact 1 — the launcher cannot open the touchscreen node (EACCES)

The panel enumerates as `"FTS3528:00 2808:1015"` → `event5` (and `"… UNKNOWN"` → `event7`). The app
runs as the seat user `deck`, and:

    crw-rw---- root input  /dev/input/event5     # touchscreen — group 'input', no ACL
    crw-rw----+ root input /dev/input/event10    # gamepad — ACL user:deck:rw-

`input` contains only `brltty`. udev's `70-uaccess.rules` grants the active-seat ACL to **joysticks
only** (`ENV{ID_INPUT_JOYSTICK}=="?*", TAG+="uaccess"`); keyboards, mice and touchscreens
deliberately get none (the standard defence against a seat program keylogging the console). So
`open("/dev/input/event5")` as `deck` returns **EACCES**.

**The Flatpak does not help.** `--device=input` (`devices=dri;input;`, verified on the installed app)
bind-mounts the nodes into the sandbox — `ls /dev/input` shows `event5` — but does not change their
owner, group or ACL. The kernel still sees `root:input 0660` with `deck` not in `input`, and `open()`
still returns EACCES. `--device=input` changes visibility, not ownership.

## Fact 2 — a uinput panel is not routed by gamescope

A virtual multitouch device created via uinput and tapped produces **zero** page clicks, with or
without `INPUT_PROP_DIRECT` (tested both ways; with the prop bit, also zero `pointerdown`s).
gamescope's touch handling does not pick up a uinput-created panel, so touch delivery cannot be
faked headlessly for an L2 test.

## Fact 3 — a successful grab does not block touch

- **No process reads the touchscreen.** An fd scan of every process found nobody holding `event5`,
  `event7`, or the panel's `hidraw1` (`HID_ID=0018:00002808:00001015`). `gamescope-wl` holds zero
  input-device fds. (The only evdev holders: our launcher on the gamepad `event10`, and
  `steamos-powerbuttond` on `event1`/`event3`. Steam holds `hidraw2`, a controller.)
- **Physical test, human at the panel, app foregrounded through Game Mode.** With `deck` granted the
  nodes by `setfacl`: no grab → 4 clicks, 7 pointerdowns from ~6 taps, and a tap opened a video. With
  `EVIOCGRAB` held successfully on BOTH `event5` and `event7` → 12 clicks, 14 pointerdowns from ~6
  taps, and the page navigated. **Touch went straight through an exclusive grab.**

gamescope consumes the panel through a channel an evdev grab cannot reach and re-emits it as pointer
input (hence `pointerdown`/`click`, never `touchstart`, in its default mode). Granting `deck` the node
with a udev rule would therefore fix nothing.

## What replaced it: `disable_touch` (default on) — touch made inert at two layers

The goal is "touch must not create a bad experience": under gamescope a finger is a cursor that
moves and taps that click, navigating YouTube by accident.

- **Option A — page-level swallow (`config/no_pointer.js`, injected as a sticky script when
  `disable_touch`).** Capturing listeners on `window`/`document` at document-start call
  `stopImmediatePropagation()` + `preventDefault()` on every pointer/mouse/touch event before
  Leanback sees it. Leanback is D-pad/key driven and our gamepad input is key events, so nothing is
  lost. Works regardless of gamescope's mode.
- **Option B — gamescope hover (`launcher/src/touchmode.cpp`, `TouchModeGuard`).** gamescope's touch
  click mode is the **global, Steam-managed root atom `STEAM_TOUCH_CLICK_MODE`**; gamescope 3.16 has
  **no per-window override** (the binary interns only that atom). Mode **0 = hover**: the cursor moves,
  nothing clicks. The guard writes the atom every tick (750 ms) while **our** `content_shell` window
  has input focus (xcb `XGetInputFocus` + WM_CLASS walk), never while another window has it. It must
  write unconditionally — Steam re-asserts its own value on focus changes and gamescope acts on the
  `PropertyNotify`, not the stored value (`touch-gestures.md` §7.0.2). Best-effort: no libxcb / no
  `DISPLAY` / no atom ⇒ it logs once and does nothing, and Option A still covers.
- **The cursor is hidden.** gamescope composites the client's X cursor, so `cursor: none` makes it
  draw nothing. `no_pointer.js` sets `*{cursor:none}` through a constructable stylesheet
  (`adoptedStyleSheets`), because youtube.com/tv's CSP drops a `<style>` tag and the universal rule
  is needed to beat per-element `cursor:pointer`. gamescope offers no "ignore touch" mode
  (`-noTouchPointerEmulation` is unsupported on Xwayland), so this is the whole answer.

**Measured on-Deck 2026-07-10 (OLED, Game Mode, user at the panel):** at mode 0, taps and drags
produced **45 mousemoves, 0 clicks / 0 pointerdowns / 0 touchstarts**, and nothing navigated — touch
reached the app (a real positive control) yet produced no action. The cursor vanished on touch.

**Which X display.** The SteamOS gamescope session runs `--xwayland-count 2`: **Steam is on `:0`, a
Steam-launched game on `:1`.** Our `content_shell` and the atom the guard writes are on **`:1`** when
launched from Steam; the guard uses the launcher's own `DISPLAY`. An out-of-band check on `:0` sees
only Steam's window and wrongly concludes B never engaged — verify on the display where
`xdotool getwindowfocus getwindowclassname` returns `chromium-content_shell`
(`tests/deck/test_touch.py` scans `:0`–`:2`). An SSH launch that forces `DISPLAY=:0` puts the guard on
the wrong display (A still works) — a harness artifact, not a product bug.

The opposite policy, `touch_gestures`, holds mode 4 and routes real touch to a gesture recogniser;
the two are mutually exclusive (`touch-gestures.md` §3).

**Tests:** `launcher/tests/touchmode_test.cpp` (focus matcher; the write decision depends on focus
alone), `tests/deck/test_touch.py::test_touch_cannot_navigate_leanback_under_either_policy`,
`::test_gamescope_touch_mode_matches_the_configured_policy` (skips when our window is unfocused).
LCD untested.
