# Deckback — Test Plan

Companion to `.internal/TASKS.md` (what to build) and `.internal/steamdeck-cobalt-youtube-plan.md`
(why). This file answers: **what is actually proven, what merely compiles, and how we prove the
rest on real hardware.** The engine pin is `DEPS.pin`.

---

## 0. Doctrine — hardware-first TDD

The engine is Chromium, the UI is a server-side web app we do not control, and the platform is a
handheld running a compositor we cannot configure. **Almost nothing important about this project is
provable on a workstation.** A green `ctest` run says our pure functions are pure; it says nothing
about whether Leanback moved focus, whether gamescope let go of the touchscreen, or whether the GPU
decoded a frame.

So the loop is:

1. **Write the paranoid test first, and make it fail on the real device.** A test that has never
   failed on hardware has never tested hardware. If it passes before the feature exists, it is
   asserting on the wrong thing.
2. **Implement or enable the feature** (launcher + config; engine patch only as last resort).
3. **Prove it with the same automated test**, from a clean boot, twice.
4. **Register the result** — `.internal/findings/` (durable vs `milestones/<pin>.md`, currently `m138.md`) with a date.
   An undated "works" is not a result.

Three standing rules, carried over from the repo's no-guessing policy:

- **Never assert on a value we chose.** Asserting the launcher dispatched `ArrowDown` proves the
  launcher. Asserting `document.activeElement` *changed* proves Leanback.
- **Negative controls are mandatory** for anything undocumented. When probing which keycodes the TV
  app honors, inject codes we believe are inert (CEA `461`, `415`) and require them to do nothing.
  Without that, "the key worked" is unfalsifiable.
- **Flake retries must not mask defects.** Retry only on transport errors (SSH/CDP
  `ConnectionError`, `TimeoutError`), never on `AssertionError`.

---

## 1. Test tiers

| Tier | Where | Runs today? | Gates |
|---|---|---|---|
| **L0** unit | host, no hardware | ✅ `just launcher test` (26 C++ cases in four grouped binaries + four node JS tests) + `just test-harness` + `just flatpak-lint`, CI on every push | pure logic, and the packaging contract |
| **L1** headless | Xvfb + CDP in the build container | ✅ `just smoke` | boots, Leanback renders, TV UA holds *and survives navigation*, AV1 steering holds without over-reaching onto VP9/H.264 (R1/R2 canary) |
| **L2** on-Deck automated | workstation drives Deck over SSH + CDP (+uinput) | ✅ `just test-deck` (`tests/deck/`) on the OLED unit — gate suite 16/17 (the A-button test is state-dependent) and `test_osd.py` 8/8, 2026-10-21 | everything that matters |
| **L3** conformance | self-hosted `js_mse_eme` against our build | ✅ `just cert conformance-test` — 45/46, one documented expected failure (2026-07-10) | MSE/EME/codec/decode-perf |
| **L4** manual / instrumented | human, camera, multimeter | ❌ never run | voice-in-a-room, input latency, battery |

**L2** runs against a Deck that is already running the app. Leanback's "Who's watching" account
picker is detected and skipped as an environment gap; there is no known-state fixture yet, so a test
can still be thrown by whatever page the previous one left (the A-button failure above). Its pure
helpers are covered at L0 (`tests/harness/test_deck_lib.py`).

**L3:** `just cert conformance-test deck` completes all 46 tests — **45 PASSED, 1 FAILED**; the
failure (`MediaElementEvents`) is a property of the harness, not of our engine (finding F11).
`tests/cert/expectations/conformance-test.json` records that verdict. The other suites have no
baseline, and `cert.py` exits **3** for them rather than passing a run it has no opinion about. L3
runs **headless in the build container, without a GPU** — it exercises MSE, EME and codec
*correctness*, not the Deck's VA-API path, and it says nothing about Leanback. `just cert-deck` runs
the same suites on the Deck in its own page target (finding F15). **Passing `js_mse_eme` is not
YouTube certification** (`docs/legal.md`).

**`just deck-ci`** chains the on-Deck tiers with no human: deploy → launch → `gate` → `cert`;
`--full` adds `probe`, `power` and `soak`, and teardown always runs. It aggregates exit codes rather
than concatenating them — a regression (2) outranks a later SSH drop (4), only transport is retried,
and **a run in which nothing adjudicated exits 3, never 0**. All of that judgement lives in
`scripts/lib/deckci.py`, which is pure and mutation-tested at L0. It has never run against a Deck
(`just deck-ci --dry-run` prints the plan and touches nothing).

**`just power` and `just soak` are closed-loop:** power refuses to sample unless the video is playing
*and* the decoder is VA-API; soak requires `currentTime` to advance after every resume. Results
(OLED): **P4 power ≤~9 W met** — `just power` averaged **5.6 W** under HW-decode VP9 playback
(2026-07-10). **P6 met** — `just soak` (10 `rtcwake` cycles) kept the app alive and the video
advancing across every resume, and the audio-restored and no-screen-dim clauses were checked by hand
(2026-07-10).

**What `just soak` does NOT test.** `rtcwake -m mem` writes `/sys/power/state` directly and
**bypasses logind**, so no `PrepareForSleep` is sent and the launcher's suspend/resume hooks
(checkpoint, network wait, resume nudge, delay lock) never run — soak proves only that the *engine*
survives a kernel suspend. Real sleeps (power button, Steam's menu) go through logind; those hooks
were driven by hand (§2.B, "Resume hook"). An SSH session cannot request a logind suspend without
interactive polkit auth.

---

## 2. Status matrix — the honest picture

### A. Verified on real hardware
All on the **OLED (Sephiroth)** unit. Dates are the finding dates.

| What | Evidence |
|---|---|
| TV UA → interactive Leanback (not cast, not desktop) | S0.2, `m138.md` |
| Leanback renders under gamescope Xwayland, real RADV GPU process | S0.4 |
| **Hardware decode (M138):** `kVideoDecoderName=VaapiVideoDecoder`, `kIsPlatformVideoDecoder=true` **and** a clean pixel verdict (0.0% green-band rows), real Leanback VP9 | 2026-07-10; re-confirmed by the L2 gates `test_hardware_decode_is_vaapi` + `test_video_is_not_corrupt` 2026-10-21; `m138.md` |
| **P4 power:** 5.6 W mean under HW-decode VP9, on battery (≤~9 W gate) | 2026-07-10, `m138.md` |
| **P6 suspend/resume:** `just soak` 10 cycles, app alive + `currentTime` advancing after every resume; audio-restored + no-dim checked by hand | 2026-07-10 (engine only — §1) |
| CDP-injected keys are **trusted** (`isTrusted=true`); arrows/Enter move + activate focus | S0.6 |
| Gamepad: real pad **and** synthetic uinput pad both drive focus; multi-device merge | 2026-07-08 |
| Hotplug: pad attached *after* startup is adopted, no teardown of open pads | 2026-07-09 |
| **L2/R2 chapter seek** (`chapter_seek.js`, fixed-skip fallback) — the triggers arrive as analog `ABS_Z`/`ABS_RZ` | 2026-07-13, `durable/input-ux.md` §18 |
| Captions: the View toggle (`toggle_captions.js`) flips CC state and selects the SteamOS system language | 2026-07-22 |
| Persistent profile survives Flatpak reinstall + Game Mode relaunch | 2026-07-09 |
| Deep link `app://<id>` → `#/watch?v=<id>`, `readyState=4` | 2026-07-08 |
| Audio out + host-side mute recovery (live muted stream recovered) | 2026-07-09 |
| Game Mode Flatpak launch + `steamos-add-to-steam` | 2026-07-08 |
| **Touch inert by default** (`disable_touch`): at gamescope hover a tap produced 45 mousemoves and 0 clicks, no navigation | 2026-07-10, `durable/touch-lock.md` |
| CDP-injected toast and controls card render over real Leanback (with a Trusted Types policy) | 2026-07-10, `m138.md` page constraints |
| **Real `wl_touch` reaches Blink at gamescope touch mode 4** — 9 taps → 9 `touchstart`+`touchend`, per-finger ids, page-space coords, left/right thirds separable | `just touch-probe`, 2026-07-31, `durable/touch-gestures.md` |
| **Multitouch is delivered** — the router's `multiFinger` counter reached 5 in 48 sequences of real use, on a device reporting `maxTouchPoints: 0` | 2026-07-31; retires `input-ux` §3 |
| **Touch gestures end to end** — swipe (one move per gesture), tap→select/pause, double-tap→seek, hold→2×, Y/R4 toggle with toast+rumble, tuned across two rounds of hardware feedback | 2026-07-31, OLED |
| **The TVHTML5 player exposes rate + quality ladders during playback** — `[0.25 .. 2]`, `["hd720","large","medium","small","auto"]`; `<video>.playbackRate` holds through real playback | P12.0b, `just feature-probe player` |
| **Docked: the external mode never reaches the nested Xwayland** — 1920x1200 panel attached, gamescope screen stays 1280x800, TV shows the app mirrored+upscaled | P12.0c, `just feature-probe dock` |

### B. Implemented, host-tested — hardware coverage partial or none
The unit tests are good tests of the wrong layer. The last column says what hardware has not shown
(and, where it has run, what it showed).

| What | What the test actually proves | What is unproven |
|---|---|---|
| AV1 steering (`navigator.cpp`) | the JS returns `unsupported` for `av01` in node/vm | on-Deck Leanback playback with steering on is VP9 (`m138.md`), but no unsteered A/B shows that steering is what causes it |
| `CdmFetcher` | hash-verify + install from a `file://` URL; `usable_cdm_path` mirrors the engine patch's absolute-path rule | anything about a real CDM. Registration is in `patches/0001-…`, but **no CDM has ever loaded**: not a licence exchange, not a frame of DRM video |
| Suspend/resume (`player_test`) | the reload-vs-nudge branch is chosen correctly | core survival is verified (§2.A, P6); the audio-restored + no-dim clauses were checked by hand, not automated |
| `just soak` / `just power` closed loop (`test_deckctl`) | paused→ENV vs stalled→ASSERT vs rewound→"restarted"; a silent `Media` domain is a broken probe, not software decode; EPP is reported, never gated | both ran on-Deck (§2.A); EPP was unchanged across resume on that build, so the "EPP moved" path has never met hardware |
| Log rotation, SIGTERM exit 0 | file sink + signal handling | Steam "Close game" in Game Mode |
| `make_rumble_effect` (`overlay_test`) | the `ff_effect` struct is filled correctly and reuses its slot id | that the Steam Input virtual pad exposes `FF_RUMBLE`, or accepts `EVIOCSFF` (`input-ux` §14) |
| `fast_scroll` (`input_test`) | direction, dominant-axis tie-break, analog rate curve, axis saturation, the CDP repeat floor, hostile configs | that the pad emits `ABS_RX`/`ABS_RY` at all, or that Leanback keeps up with a 45 ms arrow cadence |
| Error page (`errorpage_test`) | a failed `Page.navigate` is *seen* as failed; backoff saturates instead of overflowing; the classifier refuses to guess; hot-swapped text is escaped | that the injected page renders under gamescope, or that an injected Enter reaches its Retry button |
| Controls card (`onboarding_test`) | rows are derived from the shipped app.json, dead controls are omitted, disabled features are not advertised, the marker is one-shot and versioned | `overlay.js` still uses an inline `<style>` block, which the M138 CSP drops: that the card paints styled, and that swallowing evdev events freezes focus behind it |
| OSD Settings menu (`osdmenu_test`, `scripts_test`) | status line / action-button set / verdict parser are correct; `osd.js`/`osd_button.js` use only the CSP-safe style path + keep-alive observer | **On-Deck 2026-10-21:** `tests/deck/test_osd.py` gates 8/8 — focus and verdicts, tab cycle and scroll, a real body swap, Menu/B open and close, input reaching Leanback after close, capture released on reload |
| Captions toast + settings (`caption_settings_test`, `js_toggle_captions`) | the caption script's state machine; the settings parser | the toast and CC on a captioned video during normal play; the OSD ▸ Settings ▸ Captions save round trip |
| `devtools_test` | RFC6455 codec against a loopback CDP server | a real Cobalt target |
| Update ref probe — launch check + *Check for updates* installs nothing (`updateprompt_test`, 2026-10-20) | ref parsing; the probe only adds availability; staged/refused commits are not re-offered; a failed deploy can be retried by Check; an unreadable repo falls back to the portal; each guard mutation-checked | **On-Deck 2026-10-21:** inside the Flatpak the probe fetched the published ref and flagged a dev bundle as older (`update: the published repo has a newer commit`); the 0.0.9 → 0.0.10 production update ran from Settings ▸ Updates (T2, `durable/self-update.md`). Still open: the T1 list in `durable/self-update.md` ★★★★ |
| Deploy progress + **Close now** (`updateprompt_test`, `osdmenu_test`) | progress text, the timeout restarting on progress, the button set | that the portal emits Running progress during a real deploy; that Close now exits cleanly under Game Mode and the relaunch runs the new commit |
| Published-repo history, bsdiff delta, recall (`test_publish_repo.sh`, L1, off-device) | with real `flatpak --user` clients against a local signed repo: delta used, two-behind updates, recall installs, `--commit`/`mask` work; a real 0.0.8 → 0.0.9 client update through a 4.2 MB delta | the first production publish: the *signed* mirror of the real repo (only an unsigned dry run touched production), Pages serving the bigger site, a Deck pulling the delta |
| Live-repo permission gate (`test_check_permissions.sh`, `test_publish_repo.sh`) | built-metadata parsing; an added bus name/device is refused with exit 2 | — (runs at publish time, not on the device) |
| Input seam — `track_axes`, `resolve_press`, `normalize_axis`, `GamepadInput::process_event` (`input_test`, 2026-10-21) | one axis tracker for the normal/OSD/card paths; a release under the first-run card is not a phantom direction; a held modifier layer owns View/Y; a SYN_DROPPED or an early release cancels the Exit hold; Game Mode's Xbox-range pad passes through `normalize_axis` bit-identical; each mutation-checked | **On-Deck 2026-10-21** (OLED, the v0.0.9 engine + this launcher): the L2 uinput D-pad test passes and the launcher logs `btn 304 -> Enter`; the virtual pad reports **stick -32767..32767, trigger 0..255** — the stick is one off the Xbox range, so `normalize_axis` rescales it (centre 0 → −1, inside the 13000 deadzone; harmless). **Still by hand:** stick/trigger feel, the Exit hold, the first-run card |
| Resume hook on the poll thread (`player_test`, 2026-10-21) | `request_resume` returns at once and the poll thread nudges; `stop()` does not wait out the probe; a suspend during the wait cancels the nudge; mutation-checked | **On-Deck 2026-10-21:** two power-button sleeps (logind) each logged `PrepareForSleep(false)` → `network reachable` ~2 s after wake → `resume nudge sent`, video advancing after. **`just soak` cannot test this hook** (§1). The pre-sleep pause now lands on real sleeps (`paused + checkpointed`, two sleeps): the engine closes the DevTools socket at `PrepareForSleep(true)` and the pause retries on a fresh session |
| logind bus reconnect (`platform_test`, **L1**: private `dbus-daemon`, 2026-10-21) | a real bus drop → backoff → reconnect → `PrepareForSleep` delivered again; the old loop fails it | a real system-bus drop on a Deck (no safe way to cause one; restarting dbus/logind ends the session). On-Deck 2026-10-21 the backend came up (`platform: logind backend live`) and delivered both power-button sleeps |
| CDP re-arm gate, `Page.enable` once, no `Network.enable` (`devtools_test`, 2026-10-21) | a navigate from another thread lands after the UA re-arm; one `Page.enable` per connection; no `Network.enable`. The UA override without `Network.enable` was checked on the shipped M138 `content_shell` against a local page: navigation header, `fetch()`, `navigator.userAgent` | **On-Deck 2026-10-21:** Leanback served the TV UI (`navigator: TV app loaded`; L2 gate suite on Leanback). `just power` **6.39 W** (PASS) — not comparable to the 5.6 W baseline (other video, brightness, SteamOS 3.8.28), so the saving from dropping `Network.enable` is **unmeasured**; it needs an A/B against v0.0.9 on the same video |
| OSD unanswered command keeps capture (`osdmenu_test`, 2026-10-21) | a dropped socket returns Unreachable and keeps capture; "gone" still releases; the reconcile tick decides | **On-Deck 2026-10-21:** `tests/deck/test_osd.py` gates 8/8. The unanswered-command path itself needs a dropped socket mid-menu, which L2 cannot cause |
| Gesture long-poll, startup gating, one-pad loss (`gestures_test`, `player_test`, JS `touch_gestures.test.js`, 2026-10-21) | the router awaits `wait()` and does not spin on an instant empty answer; the player poll waits for the app; the pause retries a dropped session; each mutation-checked | **On-Deck 2026-10-21 (dev bundle):** idle CPU 3.69% → 1.50% of a core; the launcher's playback share (freeze test) 3.0 → 1.3 points; launch → `TV app loaded` 2.28 s → 0.84 s (median of 5), no startup warnings; gestures by finger feel unchanged; removing one of two uinput pads closed only that pad; L2 gates 16 pass, 2 environment skips |
| Atomic CDM install + startup re-verify (`cdm_fetcher_test`, 2026-10-21) | a mismatched download writes nothing; the old file survives a refused install; a truncated pinned CDM is detected | anything with a real CDM (see `CdmFetcher` above) |

### C. Implemented, **zero verification** — highest risk
Code exists, no test at any tier, never exercised on a device.

- **Right-stick fast scroll** — whether Steam Input's virtual pad emits the `ABS_RX`/`ABS_RY` analog
  axes under `steam_input.vdf` has never been checked (the `test_right_stick_axes_exist` probe asks).
- **Auto-repeat acceleration** — the code path has never executed on a device.
- Mic auto-grant (`mic_autogrant`) → a live capture stream with no prompt.
- Single-instance lock on-Deck.
- `resume_reload_after_ms` threshold (ships as `0` = disabled).
- Audio hot-swap (3.5 mm / Bluetooth).

### D. Not implemented — tests to be written *before* the code (§4)
Text entry / OSK end-to-end · voice search (removed from the launcher; it never worked on hardware) ·
`scan_rewind`/`scan_forward` keys · input-latency measurement · grid art from the local
`just install` (the one-line installer writes it) · Crashpad · forced 1280×720 surface + glyph-size
check.

### E. Phase gates not yet executed
P1 30-min stability · P2 50 launch/kill cycles · P3 latency ≤ PS5 + OSK · P4 4 h battery (the ≤9 W
half is met, §1) · P5 voice search in a room · P8 install in <5 min · P9 OLED matrix.

### F. Structural constraint (not a gap to close)
**Every hardware result above is from the OLED unit, and there is no LCD unit.** None is planned
(`findings/durable/decisions.md`), so this is a permanent property of the project, not a backlog item:

- The **AV1 dispute can never be closed in-house** for the LCD. `steer_av1_unsupported` therefore
  stays default-on *regardless* of how the OLED measurement lands. An OLED result is not licence to
  enable AV1 for users we cannot test.
- **Power, refresh-rate and resume-timing numbers do not transfer** (different process node, 60 Hz
  panel, different Wi-Fi chip). Never restate an OLED number as an LCD number.
- **Touch delivery is measured on one OLED unit only** (gamescope touch mode 4, one gamescope
  version). How an LCD panel behaves under either touch policy is unknown.
- LCD ships **best-effort/untested**, stated in README + SUPPORT.md. Recruiting an LCD owner for the
  P9 beta is the only coverage path and is a scheduling dependency.

---

## 3. L2 — the on-Deck automation harness

**Topology.** The workstation is the test runner; the Deck is a dumb target. Nothing test-related is
installed on the Deck beyond what ships already (it has `python3` 3.13 + `curl`).

```
workstation                              Deck (Game Mode)
  pytest  ──ssh──────────────────────►  launch flatpak / deckback-launcher
          ──ssh -L 9222:localhost:9222─►  content_shell CDP
          ◄─ screenshots, journalctl, battery telemetry, artifacts
          ──ssh──► python3 -c 'uinput…'  synthetic pad/keyboard (evdev path)
```

**Reuse, don't rebuild.** `scripts/cdp.py` is a dependency-free CDP client and importable library
(target re-attach — Leanback tears targets down — trusted key dispatch, screenshots, `MediaState`).
No Selenium/Playwright: they violate the minimal-deps rule and cannot run in the build image.

Two protocol details, both from the CDP docs:
- Chrome ≥111 requires `Host: localhost` on the `/json` endpoints. **Tunnel; never hit the Deck's IP
  directly.** (`cdp.py` talks to `127.0.0.1`, so it is correct by construction.)
- `Input.dispatchKeyEvent` `modifiers` is a bitfield: `Alt=1 Ctrl=2 Meta=4 Shift=8` (`devtools.cpp`
  passes it through as `mod::`).

**Layout**

```
tests/deck/
  conftest.py          # ssh tunnel (session), cdp client (function), deck-absent skip, artifacts
  lib/ssh.py           # SSH access + the transport error taxonomy
  lib/uinput.py        # synthetic gamepad + keyboard over ssh
  lib/probes.py        # capability + telemetry probes (§5), decoder and pixel verdicts
  test_capabilities.py # right stick, analog triggers, rumble, uinput access
  test_input.py        # the evdev → CDP path and Leanback's response to it
  test_media.py        # codecs, decoder identity, display corruption, Widevine
  test_osd.py          # the in-app OSD menu, over CDP then via uinput
  test_touch.py        # touch policy: page events swallowed, gamescope touch mode held
  test_ux.py           # controls card, toast and error page render and respond
```

**Fixtures / hygiene**
- Skip cleanly when no Deck; `--no-skip` makes a missing Deck an error for the unattended runner.
- Session-scoped `ssh -N -L 9222:localhost:9222 $DECK_HOST` subprocess; function-scoped CDP client.
- On failure, dump `Page.captureScreenshot` PNG + `journalctl --user` + the Flatpak's app log to
  `artifacts/`.
- Never rerun an `AssertionError` (see §0). `pytest-rerunfailures` with
  `only_rerun=["ConnectionError","TimeoutError"]` is the intended transport retry; it is not
  installed yet.
- Every test should start from a known state — fresh profile or explicit navigate, never "whatever
  the last test left behind". There is no known-state fixture yet (HARNESS §5).

**uinput gotchas** (they will cost a day otherwise): never include `EV_SYN` in the capability dict
(device creation fails); every `EV_ABS` axis needs an `AbsInfo`; `/dev/uinput` wants a udev rule
(`KERNEL=="uinput", MODE="0660", GROUP="input"`) not root; allow ~100–500 ms settle before the
consumer sees the new `eventN`.

> **Steam Input and a new uinput pad:** Steam Input adds a second Xbox-360 wrapper node for a
> *newly created* uinput pad, and the launcher merges both without duplicate keys
> (`findings/durable/hardware.md`). Prefer CDP key injection for behavioral
> assertions (it bypasses evdev and Steam entirely) and use uinput **only** to exercise the
> evdev→launcher path itself.

**Recipes:** `just test-deck [expr]` → pytest over SSH; `just cert [suite]` → §6.

---

## 4. Writing tests before the feature (the D-list)

Each future feature gets its failing test first. The assertion must observe **Leanback's** state, not
ours. Rows for features that have since shipped stay as the L2 test they still owe.

| Feature | The test that must fail first |
|---|---|
| **Text entry** | With search focused, inject `'a'` (VK 65 + `text:"a"`); assert the field `.value` gains `"a"` **and** `beforeinput`/`textInput` fired. If it cannot pass, the grid OSK is D-pad-only and that is itself the finding. |
| **OSK erase** | Inject `Backspace`(8) with text present; assert a character is deleted **and** the view did not pop. Prediction from `key.h`: this **fails** — `kSbKeyBack == kSbKeyBackspace == 8`. Then `Delete`(46) becomes the candidate. |
| **`scan_rewind` / `scan_forward`** | Add `MediaRewind`(227)/`MediaFastForward`(228) to `devtools.cpp:kKeys` (it cannot dispatch them today), inject during playback, assert `currentTime` moves; negative-control CEA `412`/`417` must **not** move it. |
| **Voice search** | First assert the mic button *exists* under our Cobalt UA (`typeof window.h5vcc`, DOM query) — our own UA may hide it (findings §13.2). Then a trusted `Input.dispatchMouseEvent` on its rect calls `getUserMedia` with **no** prompt and the track is **live and non-silent** (assert audio levels, not absence-of-error). Then repeat **with a video playing**: speaker bleed at ~15 cm is the real case, and a silent-room pass proves nothing (§13.3). (Not a key — §8.2.) |
| **Hold-to-talk** | Press-and-hold opens the mic, release closes it, and a short tap does **not** open it (debounce). |
| **L2/R2 modifier layer** | With LT held, assert D-pad Left emits the layer's key and **not** `ArrowLeft`; released, assert the plain arrow returns. (`keymap_lt`/`keymap_rt` ship empty.) |
| **Context keymap layers** | With player open, `B` must exit the player; inside the OSK, `B` must **not** exit search. |
| **Error/retry page** | Sever DNS; assert the launcher's own focusable retry page renders and **no** `net::ERR_*` Chromium interstitial is reachable. |
| **First-run overlay** | Fresh profile → overlay present, dismissable with a controller button, and absent on second launch. |
| **Input latency** | Log `evdev-receipt → CDP-ack` per key; assert p95 under a budget. Calibrate once against a 120/240 fps camera (L4) — the gate says "≤ PS5" with no method, which is unmeasurable as written. |
| **Widevine** | `navigator.requestMediaKeySystemAccess('com.widevine.alpha', …)` must reject (`NotSupportedError`) while no CDM is installed, and resolve once a user-installed CDM is registered by `patches/0001-…`. |
| **Focus traps** (Verified gate) | For each surface, press every direction from every focusable element; assert focus always changes or is deliberately absorbed, and nothing becomes unreachable. |
| **Glyph size** (Verified gate) | Screenshot + measure the smallest rendered glyph ≥ **9 px @1280×800** (12 px recommended). |

---

## 5. Capability probes — "what does the page actually report?"

Two channels; they answer different questions and are easy to confuse.

**Page-level** (`Runtime.evaluate`) — what YouTube *believes* about us. Guards R1 and the AV1 steering:
- `navigator.userAgent` — the TV UA survived (`Network.setUserAgentOverride` is sticky across
  Leanback's target teardown).
- `MediaSource.isTypeSupported('video/mp4; codecs="av01…"')` → must be `false`;
  `vp9`/`vp09`/`avc1` → `true`. Same for `HTMLMediaElement.canPlayType` and
  `navigator.mediaCapabilities.decodingInfo`.
- `navigator.requestMediaKeySystemAccess('com.widevine.alpha', …)` → P7 state.
- `video.getVideoPlaybackQuality()` → `{totalVideoFrames, droppedVideoFrames, corruptedVideoFrames}`.
  **Frame counts only — no decoder name.**

**CDP `Media` domain** — what the *engine* actually did. This is the supported way to get the
decoder identity, and it is the P4 gate:
- `Media.enable`, then subscribe `Media.playerPropertiesChanged` → `{playerId, properties:[{name,value}]}`
  (name/value are plain strings, from `media/base/media_log_properties.h`).
- Read **`kVideoDecoderName`** and **`kIsPlatformVideoDecoder`**; also useful: `kResolution`,
  `kFramerate`, `kVideoPlaybackFreezing`, `kSetCdm`/`kIsCdmAttached`.
- **P4 hardware-decode gate:** pass iff `kIsPlatformVideoDecoder == "true"` **and**
  `kVideoDecoderName` contains `"Vaapi"` (match the substring — an older path may stringify as
  `VaapiVideoDecodeAccelerator`). **Fail** on `Dav1dVideoDecoder` (AV1 software — must never appear),
  `VpxVideoDecoder`, `FFmpegVideoDecoder`. Cache the latest non-null value per `playerId`. The
  decoder name is half the gate: a composited screenshot must also pass
  `probes.py:video_corruption_verdict` (no green-band rows), because a hardware decoder can be
  selected and still paint corrupt frames.

**Do not scrape `chrome://media-internals`.** It is a WebUI over the same `MediaLog` stream with no
stable API. The `Media` domain is the supported surface for identical data.

*This also gives us a real test for the AV1 dispute (`findings/durable/hardware.md`): force an AV1
stream, and assert on `kVideoDecoderName` rather than on libva's advertised profiles.*

---

## 6. L3 — Google/YouTube conformance suites

**The public endpoints are dead** (checked 2026-07-09 by fetching them):

| Endpoint | Status |
|---|---|
| `ytlr-cert.appspot.com/*`, `yts.devicecertification.youtube/*` | HTTP 200 but a **2.4 KB sunset stub** ("not actively maintained since 2021"); no harness JS |
| `yt-dash-mse-test.commondatastorage.googleapis.com/unit-tests/{2018,2019,2021}.html` | **404** |
| YouTube device-certification requirement PDFs | **404** — moved behind the partner portal |
| **`github.com/youtube/js_mse_eme`** | **LIVE** — full harness source, archived read-only 2026-04-19 |
| `storage.googleapis.com/ytlr-cert.appspot.com/test-materials/media/*.mp4` | **LIVE** — test vectors still served |

**So: vendor `youtube/js_mse_eme` at a pinned commit and self-host it.** The media vectors load from
the still-live GCS bucket unchanged (`MEDIA_PATH`/`CERT_PATH` in `harness/util.js`). Self-hosting
also silences the harness's `qual-e.appspot.com` phone-home, which only fires when the page host
contains `appspot.com`/`googleapis.com`.

**Suites available** (`harness/testTypes.js`): `conformance-test` (MSE, default), `msecodec-test`,
`encryptedmedia-test` (EME, incl. Widevine), `progressive-test`,
`playbackperf-{sfr-vp9,sfr-h264,sfr-av1,hfr}-test`, `playbackperf-widevine-*`.

**Driving it headlessly** — the harness is URL-driven and self-reports to globals, no UI needed:

```
http://<self-host>/main.html?test_type=conformance-test&command=run&timeout=40000
```
`command=run` auto-runs on load (`command=run:1,2,3` for a subset); `stoponfailure=true`, `loop` also
exist. Then poll over `Runtime.evaluate`:

```js
JSON.stringify(window.getTestResults())        // {pass:{…}, fail:{…}} grouped by category
```
or walk `window.globalRunner.testList[i].prototype.{outcome,name,lastError}` with
`TestOutcome = {UNKNOWN:0, PASSED:1, FAILED:2, OPTIONAL_FAILED:3, TIMEOUT:4}` — poll until no test
is still `UNKNOWN`, then assert zero `FAILED`.

**What it buys us, mapped to our open questions:**
- `msecodec-test` + `playbackperf-sfr-av1-test` → an *independent* check on the AV1 steering and the
  AV1 hardware dispute.
- `encryptedmedia-test` → a Widevine gate that does not depend on a YT rental (P7).
- `playbackperf-*` → the dropped-frame / decode-performance evidence P4's battery gate assumes.

**Caveat, stated plainly:** passing `js_mse_eme` is *not* YouTube certification and confers no
status. It is the same harness Google used to certify devices, which makes it an excellent
regression net — nothing more. Do not let it appear in user-facing material as "certified."

**Cobalt's own suites** (for completeness):
- `cobalt/black_box_tests` — **removed on Chrobalt trunk** (present only on the `25.lts` Starboard
  branch; requires the Starboard app launcher). Do not plan around it.
- `starboard/nplb` — still present and compiles, but Chrobalt plays video through the **Chromium**
  media pipeline, so NPLB no longer exercises real decode. Reduced, non-zero value (DRM/system
  contract).
- `media_unittests` (`media/BUILD.gn`) and Blink/WPT `media-capabilities/` + `encrypted-media/` run
  against the `content_shell` we already build — `decodingInfo.any.js` directly exercises the exact
  surface our AV1 steering overrides.

---

### 6.1 Certification automation — removing the human

Goal: `just cert` returns an exit code and an artifact, with nobody watching. The harness is already
URL-driven and self-reporting, so the automation problem is not "drive the tests" — it is **hosting,
adjudication, and hang detection**.

**Topology — serve on the workstation, expose to the Deck over a reverse tunnel.**

```
workstation                                    Deck
  python3 -m http.server 8000  (pinned js_mse_eme)
  ssh -R 8000:localhost:8000 $DECK_HOST ──────► engine sees http://localhost:8000/main.html
  ssh -L 9222:localhost:9222 $DECK_HOST ◄────── CDP
```

The reverse tunnel is not a convenience, it is **load-bearing for two independent reasons**:

1. **EME needs a secure context.** `requestMediaKeySystemAccess` is gated on a potentially-trustworthy
   origin. `http://localhost` qualifies; `http://192.168.x.y:8000` does **not**. Serve over the LAN IP
   and every EME suite fails at the first call, for a reason that has nothing to do with our engine.
   (Fallback if the tunnel is ever impossible: `--unsafely-treat-insecure-origin-as-secure=<origin>`
   on a throwaway profile — never on a shipping build.)
2. **It silences the phone-home.** The harness POSTs results to `qual-e.appspot.com` only when the
   page host contains `appspot.com`/`googleapis.com`. A `localhost` origin never triggers it.

Assert both preconditions *before* running anything, so a misconfiguration fails loudly instead of
masquerading as a product bug:

```js
({secure: window.isSecureContext, host: location.host})   // must be {true, "localhost:8000"}
```

**Media vectors.** The harness fetches them from the still-live GCS bucket, so an unattended run has
a hard dependency on a third-party host that has already sunset its sibling endpoints. Mirror them
once into a gitignored cache, **SHA-256-pinned**, and serve them from the same origin — the same
posture as `CdmFetcher`: fetch, verify, never redistribute. Preflight a `HEAD` on each vector and
fail with a distinct exit code, so "Google took the bucket down" never reads as "our decoder broke."

**`scripts/cert.py` stages** (reusing the `cdp.py` client, no new deps):

1. **Preflight** — harness at the pinned commit; vectors present + hashes match; port free.
2. **Serve + tunnel**; assert `isSecureContext` and `location.host`.
3. **Launch or attach** to the engine (`remote-run.sh` does a headless Game Mode launch); on the
   Deck, in its own page target.
4. **Navigate** to `main.html?test_type=<suite>&command=run&timeout=<ms>`.
5. **Poll** `window.globalRunner.testList[].prototype.outcome` until nothing is `UNKNOWN(0)`.
6. **Scrape** `getTestResults()` plus per-test `name`/`lastError`.
7. **Adjudicate** against the baseline (§6.2).
8. **Emit** JUnit XML + JSON + a screenshot; exit non-zero only on a *regression*.

**Hang detection is mandatory.** A conformance harness that wedges on one test will otherwise hold a
runner forever. Two independent deadlines: a per-suite wall clock, and a **no-progress watchdog** —
if the count of non-`UNKNOWN` tests does not increase within N seconds, abort, screenshot, and dump
which test was in flight. `command=stoponfailure=true` is available but should stay **off** in CI:
we want the whole result vector, not the first failure.

### 6.2 Adjudication — a committed baseline, or this gate dies in a week

Some tests fail legitimately (`OPTIONAL_FAILED`), and some will fail because of known gaps of ours.
A suite that is red on day one gets ignored by day three. So:

- Commit `tests/cert/expectations/<suite>.json`: `{test_name: PASSED|FAILED|OPTIONAL_FAILED|TIMEOUT}`.
- **Fail the run only on a regression** — a test that was `PASSED` and is now not.
- A test that newly *passes* is not a failure; it prints `baseline update available`. Never
  auto-update the baseline in CI, or the gate silently ratchets down.
- Record the js_mse_eme commit + vector hashes in the result JSON. A baseline is meaningless without
  knowing which harness produced it.

**Flake isolation without masking.** Re-run only the failed indices (`command=run:3,17,42`). A test
that passes on re-run is recorded **`flaky`**, not `pass`, and flakes are reported. Applying §0's
rule: retry transport, never adjudication.

### 6.3 Two cert tiers

| Recipe | Where | Suites | Cadence | Gates |
|---|---|---|---|---|
| `just cert` | workstation, Xvfb + `content_shell` | `conformance-test`, `msecodec-test`, `encryptedmedia-test` (ClearKey) | every PR | MSE/EME/codec regressions — **no Deck needed** |
| `just cert-deck` | real hardware over SSH | above + `playbackperf-*` | nightly + pre-release | adds real GPU decode |

Most conformance value needs no Deck at all: MSE/codec correctness is engine behavior, and it runs
headless in the build image next to `just smoke`. That is what makes the gate cheap enough to keep.

**`playbackperf-*` is a trend, not a gate.** Its results depend on TDP, refresh rate, and thermals —
it will not be deterministic on a handheld. Record to CSV, alert on a regression band, never fail a
build on it. Pair it with the §5 `Media`-domain probe so a perf drop is attributable: a
`playbackperf` regression with `kVideoDecoderName` flipped from `Vaapi…` to `Dav1dVideoDecoder` is a
steering bug, not a thermal one.

### 6.4 Where the human still is, and how each one leaves

| Step | Human today | Removal |
|---|---|---|
| Launch the app in Game Mode | yes | `scripts/remote-run.sh` launches the Flatpak headlessly |
| Start playback | `power.sh` refuses to sample until a video is playing | the cert harness plays its own media; `--autoplay-policy=no-user-gesture-required` is already set |
| Read the results table | eyeball it | `getTestResults()` scrape |
| Decide pass/fail | judgment | the committed baseline (§6.2) |
| **YouTube sign-in** | — | **not needed.** The cert harness is account-independent; it never loads `youtube.com/tv`. This is why cert automates cleanly while an end-to-end Leanback test does not. |
| **Widevine CDM install** | manual, opt-in | **stays manual, by design.** CI must never fetch Google's CDM (`docs/legal.md`). So the automated gate runs **ClearKey only**; Widevine key systems are `expected-fail` in the baseline — the registration patch is in, but CI never has a CDM to register. Automating this one would mean redistributing the CDM — we won't. |

### 6.5 Failure modes the harness must distinguish

Each gets a distinct exit code, because "cert failed" is useless at 3 a.m.:

| Symptom | Real cause | Guard |
|---|---|---|
| every EME test fails instantly | insecure origin | assert `isSecureContext` in preflight |
| vectors 404 | GCS bucket changed | `HEAD` + SHA-256 preflight |
| results POSTed outward | served from an `appspot`/`googleapis` host | assert `location.host == localhost:*` |
| suite never finishes | wedged test | no-progress watchdog + in-flight test name |
| video never plays | autoplay policy | assert `!video.paused` early |
| target vanishes mid-run | Leanback-style teardown | `cdp.py` re-attaches |
| suite navigated away on the Deck | the launcher's Navigator owns its page | cert opens its own page target (F15) |

### 6.6 What this gate does *not* mean

Passing `js_mse_eme` is **not** YouTube certification, confers no status, and must never appear in
user-facing material as "certified" (see §6 and `docs/legal.md`). It is a regression net built from
the same harness Google used to certify devices. That is a strong claim on its own and does not need
inflating.

## 7. CI topology

| Runner | Tiers | Trigger |
|---|---|---|
| GitHub hosted (`lint.yml`) | L0 + lint via `just preflight` (shellcheck, harness suite, clang-format, gcc+clang launcher build, gn-args) | every push/PR |
| GitHub hosted (`release.yml`) | publishes the Flatpak repo to GitHub Pages from a release's bundle (no build) | release published |
| Workstation (manual) | L1 `just smoke`, L3 `just cert`, engine + release builds | by hand |
| Workstation + Deck (manual) | L2 `just test-deck`, `just power`, `just soak`, `just cert-deck` | by hand |
| Human | L4 | pre-release only |

**Planned, not built:** a self-hosted runner (workstation, sccache) running `just smoke` nightly as
the **R1/R2 canary** against production `youtube.com/tv`, and a Deck-attached runner for nightly L2 +
`just cert-deck` (`just deck-ci` is the entry point; it has never run against a Deck). The cert split
stands either way: MSE/codec/EME-ClearKey suites are *engine* behavior and run headless in the build
image, so the Deck is reserved for what genuinely needs hardware (real GPU decode, `playbackperf-*`).

`just smoke` runs the §5 capability probes (`--assert-ua`, `--assert-av1-steering`), so AV1
steering breakage is caught by the canary rather than at release. It injects
`config/scripts/av1_steering.js` — the same file the launcher compiles in — and first asserts, on
the unsteered engine, that AV1 *is* supported. Without that baseline the whole check is vacuous on
any build that lacks an AV1 decoder, which is precisely the shape of F1.

---

## 8. Anti-goals

- **No test that only proves our own code echoed our own constant.** (See §0.)
- No Selenium/Playwright/Boost-class dependency; `cdp.py` + stdlib + `pytest` only.
- No test that requires a signed-in account in CI. Sign-in is L4/manual — credentials never enter CI.
- No scraping `chrome://media-internals`; no dependence on undocumented `?env_` Leanback params
  (searched; no authoritative list exists — treat any such claim as unverified).
- Do not claim "certified." See §6.
