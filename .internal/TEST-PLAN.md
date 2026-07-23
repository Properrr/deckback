# Deckback — Test Plan

Companion to `.internal/TASKS.md` (what to build) and `.internal/steamdeck-cobalt-youtube-plan.md`
(why). This file answers: **what is actually proven, what merely compiles, and how we prove the
rest on real hardware.**

Status date: 2026-07-10. Pin: `DEPS.pin = a9181df8` (m114).

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
4. **Register the result** — `.internal/findings/` (durable vs `milestones/m114.md`) with a date.
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
| **L0** unit | host, no hardware | ✅ `just launcher test` (13 binaries) + `just test-harness` + `just flatpak-lint`, CI on every push | pure logic, and the packaging contract |
| **L1** headless | Xvfb + CDP in the build container | ✅ `just smoke` | boots, Leanback renders, TV UA holds *and survives navigation*, AV1 steering holds without over-reaching onto VP9/H.264 (R1/R2 canary) |
| **L2** on-Deck automated | workstation drives Deck over SSH + CDP (+uinput) | ⚠️ **ran once 2026-07-10, blocked by Leanback's guest-account gate** (`just test-deck`, 23 tests) | everything that matters |
| **L3** conformance | self-hosted `js_mse_eme` against our build | ✅ `just cert conformance-test` — 45/46, one documented expected failure (2026-07-10) | MSE/EME/codec/decode-perf |
| **L4** manual / instrumented | human, camera, multimeter | ❌ never run | voice-in-a-room, input latency, battery |

L0 and L1 are real and healthy. **L2 ran against a Deck for the first time on 2026-07-10**
(`tests/deck/`, 23 tests). Before that it had never executed: `reachable()` could not parse the
`user@host` in `DECK_HOST` and reported every Deck unreachable, so all 23 skipped (harness.md F14).
The run reached the OLED unit — **but it was compromised by Leanback's "watch as guest" account
gate**: the tests could not drive past the account wall, so almost nothing in §2 actually moves out
of "unverified" yet. It stays that way until a `conftest.py` fixture dismisses the guest gate and the
suite re-runs green against real Leanback (m114.md). Its pure helpers *are* covered at L0
(`tests/harness/test_deck_lib.py`), which is what made the earlier skip tolerable rather than a lie.

**L3 has now run for real** (2026-07-10). An earlier revision of this file said `just cert` "has
never had an engine to talk to"; that was false when written — `out/deck/content_shell` was built on
2026-07-09 — and the claim survived into a commit message. It is corrected here rather than quietly
deleted. `just cert conformance-test deck` completes all 46 tests: **45 PASSED, 1 FAILED**, and the
failure (`MediaElementEvents`) is a property of the harness, not of our engine (finding F11).
`tests/cert/expectations/conformance-test.json` records that verdict. The other suites still have no
baseline, and `cert.py` exits **3** in that state rather than passing a run it has no opinion about.

Note what "L3 passes" does and does not mean. It runs **headless, on the workstation, on SwiftShader**
— it exercises MSE, EME and codec *correctness*, not the Deck's VA-API path, and it says nothing
about Leanback. `just cert --deck` exists and has never been pointed at a Deck. And **passing
`js_mse_eme` is not YouTube certification** (`docs/legal.md`).

**`just deck-ci` (2026-07-10)** chains the on-Deck tiers with no human: deploy → launch → `gate` →
`cert`, `--full` adds `probe`, `power` and `soak`, and teardown always runs. It aggregates exit codes
rather than concatenating them — a regression (2) outranks a later SSH drop (4), only transport is
retried, and **a run in which nothing adjudicated exits 3, never 0**. All of that judgement lives in
`scripts/lib/deckci.py`, which is pure and mutation-tested at L0; the half that needs a Deck contains
no decisions. It has never met a Deck (`just deck-ci --dry-run` prints the plan and touches nothing).

`power.sh` and `soak.sh` are closed-loop as of 2026-07-09 (T5): power refuses to sample unless the
video is playing *and* the decoder is VA-API, and soak requires `currentTime` to advance after every
resume. **`just soak` ran for the first time on 2026-07-10** — `rtcwake` suspend/resume cycles with
the app alive and the video advancing across every resume; its audio-restored and no-screen-dim
clauses **also passed** on 2026-07-10, so **P6 is met** (OLED). **`just power` passed 2026-07-10** too:
**5.6 W** average under HW-decode VP9 playback (OLED), under the ≤~9 W P4 gate.

---

## 2. Status matrix — the honest picture

### A. Verified on real hardware
All on the **OLED (Sephiroth)** unit. Dates are the finding dates.

| What | Evidence |
|---|---|
| TV UA → interactive Leanback (not cast, not desktop) | S0.2, `m114.md` |
| Leanback renders under gamescope Xwayland, real RADV GPU process | S0.4 |
| VA-API decode works: `kVideoDecoderName=VaapiVideoDecoder`, `kIsPlatformVideoDecoder=true`, VP9 720p, 0/84 dropped | S0.5 |
| CDP-injected keys are **trusted** (`isTrusted=true`); arrows/Enter move + activate focus | S0.6 |
| Gamepad: real pad **and** synthetic uinput pad both drive focus; multi-device merge | 2026-07-08 |
| Hotplug: pad attached *after* startup is adopted, no teardown of open pads | 2026-07-09 |
| Persistent profile survives Flatpak reinstall + Game Mode relaunch | 2026-07-09 |
| Deep link `app://<id>` → `#/watch?v=<id>`, `readyState=4` | 2026-07-08 |
| Audio out + host-side mute recovery (live muted stream recovered) | 2026-07-09 |
| Game Mode Flatpak launch + `steamos-add-to-steam` | 2026-07-08 |

### B. Implemented, host-tested only — never touched hardware
The unit tests are good tests of the wrong layer. Each of these is **unproven where it counts**.

| What | What the test actually proves | What is unproven |
|---|---|---|
| AV1 steering (`navigator.cpp`) | the JS returns `unsupported` for `av01` in node/vm | that **YouTube then serves VP9** on-Deck |
| `CdmFetcher` | hash-verify + install from a `file://` URL; `usable_cdm_path` mirrors the engine patch's absolute-path rule | anything about a real CDM. The registration gap is now *closed in code* (`patches/0001-…`, compiled 2026-07-09) but **no CDM has ever loaded**: not a licence exchange, not a frame of DRM video |
| Suspend/resume (`player_test`) | the reload-vs-nudge branch is chosen correctly | core survival is **verified** (`just soak`, 10 cycles, 2026-07-10); the audio-restored + no-dim clauses are still unproven |
| `just soak` / `just power` closed loop (`test_deckctl`) | paused→ENV vs stalled→ASSERT vs rewound→"restarted"; a silent `Media` domain is a broken probe, not software decode; EPP is reported, never gated | that a Deck ever resumes, that `currentTime` advances after one, or that EPP actually moves — none of it has met hardware |
| Log rotation, SIGTERM exit 0 | file sink + signal handling | Steam "Close game" in Game Mode |
| Touch chord parsing | `parse_chord` string→mask | that the chord toggles anything |
| `TouchLockChord` (`input_test`) | lock is immediate, unlock needs an 800 ms hold, a repeat report never restarts it, a failed grab is reconciled | that a grab ever succeeds — the machine is correct about a lock that may not exist |
| Touch-lock toast (`overlay_test`) | the JS is well-formed and escapes quotes/backslashes/newlines; `show_toast` survives a dead engine | that the `<div>` renders, or survives Leanback's DOM management |
| `make_rumble_effect` (`overlay_test`) | the `ff_effect` struct is filled correctly and reuses its slot id | that the Steam Input virtual pad exposes `FF_RUMBLE`, or accepts `EVIOCSFF` |
| `fast_scroll` (`input_test`) | direction, dominant-axis tie-break, analog rate curve, axis saturation, the CDP repeat floor, hostile configs | that the pad emits `ABS_RX`/`ABS_RY` at all, or that Leanback keeps up with a 45 ms arrow cadence |
| Error page (`errorpage_test`) | a failed `Page.navigate` is *seen* as failed; backoff saturates instead of overflowing; the classifier refuses to guess; hot-swapped text is escaped | that the injected page renders under gamescope, or that an injected Enter reaches its Retry button |
| Controls card (`onboarding_test`) | rows are derived from the shipped app.json, dead controls are omitted, disabled features are not advertised, the marker is one-shot and versioned | that the card renders over Leanback, or that swallowing evdev events freezes focus behind it |
| OSD Settings menu (`osdmenu_test`, `scripts_test`) | status line / action-button set / verdict parser are correct; `osd.js`/`osd_button.js` use only the CSP-safe style path + keep-alive observer | that the menu renders and navigates over Leanback, that *capture ⇔ paint* holds under a real body swap, and that input is restored after the menu closes — the two-bug L2 suite (`tests/deck/test_osd.py`) exists but has **not** run on a Deck |
| `devtools_test` | RFC6455 codec against a loopback CDP server | a real Cobalt target |

### C. Implemented, **zero verification** — highest risk
Code exists, no test at any tier, never exercised on a device.

- **`TouchGuard` `EVIOCGRAB` actually starving gamescope of touch.** This is the explicit product
  requirement (input-ux §4) and the entire mechanism is an untested assumption. If gamescope grabs
  first, the feature is a silent no-op. **Test this before anything is built on top of it.**
- Touch-lock chord toggling at runtime. The lock now *announces* itself (toast + rumble + a
  deliberate 800 ms unlock hold — input-ux §14), but the announcement is driven by our own state, not
  by the kernel's: it fires whenever `EVIOCGRAB` returns success. **A human "I saw the toast" report
  is still not evidence the touchscreen went dead.** Assert on the grab — inject a tap via uinput and
  observe that gamescope does not see it. Until then the feedback may be advertising a no-op.
- The toast injection itself (does a CDP-appended `<div>` survive Leanback?) and the rumble upload
  (does the Steam Input virtual pad implement `FF_RUMBLE` on a node we may open `O_RDWR` inside the
  sandbox?). Both fail silently by design, so neither will announce its own absence.
- **Right-stick fast scroll** — whether Steam Input's virtual pad emits the `ABS_RX`/`ABS_RY` analog
  axes under `steam_input.vdf` has never been checked. Identical hazard class to the trigger-axis one
  below, and the same one-line `evtest` on the virtual pad answers both.
- **The triggers (L2/R2) dispatch nothing at all** — `scan_rewind`/`scan_forward` resolve to no DOM
  key. Separately unverified: whether `steam_input.vdf`'s `"click" → xinput_button TRIGGER_*` binding
  even produces the `ABS_Z`/`ABS_RZ` analog axes `input.cpp` reads (input-ux §12). Two independent
  reasons the same buttons could be dead; a test must distinguish them.
- **Auto-repeat acceleration** — the code path has never executed on a device.
- `toggle_captions` functionality (**mechanism changed 2026-07-22**). The old Select→`c` keydown was
  confirmed dead on-device — Leanback ignores the desktop `c` hotkey — so captions now drive the
  player's caption module over CDP (`config/scripts/toggle_captions.js`, a launcher action like the
  seeks). Unverified on-Deck: that the module toggle actually turns CC on/off on a captioned video,
  that the "no caption tracks" path reports correctly, and that the `captions_toast` feedback matches.
- Mic auto-grant → a live capture stream with no prompt.
- **Voice search end to end.** `voice_enabled` ships **false**: V0 (does Leanback render a soft-mic
  button under our Cobalt UA at all?) has never been run. The launcher's behaviour when it finds no
  button — click nothing, duck nothing, warn once — is unit-tested; that the button *exists* is not.
- Single-instance lock on-Deck.
- `resume_reload_after_ms` threshold (ships as `0` = disabled).
- Audio hot-swap (3.5 mm / Bluetooth).
- `--use-angle=gl` render smoothness + battery cost — **this blocks VA-API adoption**, the whole P4 win.

### D. Not implemented — tests to be written *before* the code (§4)
Text entry / OSK end-to-end · touch gesture layer · context keymap layers (browse/player/OSK) ·
right-stick fast traversal · first-run controls overlay · kiosk error/retry page · input-latency
measurement · Widevine `AddContentDecryptionModules` patch · rumble/haptics · grid art · Crashpad ·
forced 1280×720 surface + glyph-size check.

### E. Phase gates never executed
P1 30-min stability · P2 50 launch/kill cycles · P3 latency ≤ PS5 + OSK · P4 4 h battery / ≤9 W ·
P5 voice search in a room · P6 25× suspend/resume · P8 install in <5 min · P9 OLED matrix.

### F. Structural constraint (not a gap to close — locked 2026-07-09)
**Every hardware result above is from the OLED unit, and there is no LCD unit.** None is planned
(`findings/durable/decisions.md`), so this is a permanent property of the project, not a backlog item:

- The **AV1 dispute can never be closed in-house** for the LCD. `steer_av1_unsupported` therefore
  stays default-on *regardless* of how the OLED measurement lands. An OLED result is not licence to
  enable AV1 for users we cannot test.
- **Power, refresh-rate and resume-timing numbers do not transfer** (different process node, 60 Hz
  panel, different Wi-Fi chip). Never restate an OLED number as an LCD number.
- ⚠ **Dropping LCD concentrates the touch risk rather than removing it.** `touch.cpp` pins
  `2808:1015` (FTS3528), an id taken from a blog post about an *LCD* Deck — never measured on our
  OLED. Its capability/name fallback is the real cross-model path and is untested. **Dump the OLED
  panel's true id before trusting any touch test.**
- LCD ships **best-effort/untested**, stated in README + SUPPORT.md. Recruiting an LCD owner for the
  P9 beta is the only coverage path and is a scheduling dependency.

---

## 3. L2 — the on-Deck automation harness (to build)

**Topology.** The workstation is the test runner; the Deck is a dumb target. Nothing test-related is
installed on the Deck beyond what ships already (it has `python3` 3.13 + `curl`).

```
workstation                              Deck (Game Mode)
  pytest  ──ssh──────────────────────►  launch flatpak / deckback-launcher
          ──ssh -L 9222:localhost:9222─►  content_shell CDP
          ◄─ screenshots, journalctl, power_now, artifacts
          ──ssh──► python3 -c 'uinput…'  synthetic pad/keyboard (evdev path)
```

**Reuse, don't rebuild.** `scripts/cdp.py` is already a dependency-free CDP client with target
re-attach (Leanback tears targets down), trusted key dispatch, and screenshots. Promote it to a
library (`scripts/cdp.py` → importable `CDP` class) rather than adding Selenium/Playwright, which
would violate the minimal-deps rule and cannot run in the build image.

Two protocol details worth pinning down now, both from the CDP docs:
- Chrome ≥111 requires `Host: localhost` on the `/json` endpoints. **Tunnel; never hit the Deck's IP
  directly.** (`cdp.py` already talks to `127.0.0.1`, so it is correct by construction.)
- `Input.dispatchKeyEvent` `modifiers` is a bitfield: `Alt=1 Ctrl=2 Meta=4 Shift=8` — needed for the
  `Shift+N/P` experiments, and currently unsupported by `devtools.cpp`.

**Layout**

```
tests/deck/
  conftest.py        # ssh tunnel (session), cdp client (function), deck-absent skip, artifacts
  lib/uinput.py      # synthetic gamepad + keyboard over ssh
  lib/probes.py      # capability + telemetry probes (§5)
  test_input.py      # focus moves, every direction, no traps
  test_keys.py       # the §4 keycode spikes, with negative controls
  test_touch.py      # TouchGuard: gamescope really loses touch
  test_media.py      # decoder identity, dropped frames, AV1 steering
  test_power.py      # closed-loop: start playback, sample power_now, assert
  test_lifecycle.py  # suspend/resume, launch/kill cycles, single-instance
  test_conformance.py# L3 driver (§6)
```

**Fixtures / hygiene**
- Skip cleanly when no Deck: probe `socket.create_connection((DECK_HOST, 22), 2)`.
- Session-scoped `ssh -N -L 9222:localhost:9222 $DECK_HOST` subprocess; function-scoped CDP client.
- On failure, dump `Page.captureScreenshot` PNG + `journalctl --user` + the app log to `artifacts/`.
- `pytest-rerunfailures` with `only_rerun=["ConnectionError","TimeoutError"]` — never rerun an
  `AssertionError` (see §0).
- Every test starts from a known state: fresh profile or explicit navigate, never "whatever the last
  test left behind."

**uinput gotchas** (they will cost a day otherwise): never include `EV_SYN` in the capability dict
(device creation fails); every `EV_ABS` axis needs an `AbsInfo`; `/dev/uinput` wants a udev rule
(`KERNEL=="uinput", MODE="0660", GROUP="input"`) not root; allow ~100–500 ms settle before the
consumer sees the new `eventN`.

> **Open question worth a milestone finding:** does Steam Input in Game Mode `EVIOCGRAB`/re-wrap a
> *newly created* uinput pad (→ double input)? Unverified. Prefer CDP key injection for behavioral
> assertions (it bypasses evdev and Steam entirely) and use uinput **only** to exercise the
> evdev→launcher path itself.

**New recipes:** `just test-deck [expr]` → pytest over SSH; `just cert [suite]` → §6.

---

## 4. Writing tests before the feature (the D-list)

Each future feature gets its failing test first. The assertion must observe **Leanback's** state, not
ours.

| Feature | The test that must fail first |
|---|---|
| **Text entry** | With search focused, inject `'a'` (VK 65 + `text:"a"`); assert the field `.value` gains `"a"` **and** `beforeinput`/`textInput` fired. If it cannot pass, the grid OSK is D-pad-only and that is itself the finding. |
| **OSK erase** | Inject `Backspace`(8) with text present; assert a character is deleted **and** the view did not pop. Prediction from `key.h`: this **fails** — `kSbKeyBack == kSbKeyBackspace == 8`. Then `Delete`(46) becomes the candidate. |
| **`scan_rewind` / `scan_forward`** | Add `MediaRewind`(227)/`MediaFastForward`(228) to `devtools.cpp:kKeys` (it cannot dispatch them today), inject during playback, assert `currentTime` moves; negative-control CEA `412`/`417` must **not** move it. |
| **Voice search** | First assert the mic button *exists* under our Cobalt UA (`typeof window.h5vcc`, DOM query) — our own UA may hide it (findings §13.2). Then a trusted `Input.dispatchMouseEvent` on its rect calls `getUserMedia` with **no** prompt and the track is **live and non-silent** (assert audio levels, not absence-of-error). Then repeat **with a video playing**: speaker bleed at ~15 cm is the real case, and a silent-room pass proves nothing (§13.3). (Not a key — §8.2.) |
| **Hold-to-talk** | Press-and-hold opens the mic, release closes it, and a short tap does **not** open it (debounce). *V2 landed 2026-07-09: the `HoldToTalk` machine and `VoiceController` are covered at L0 by `voice_test` (13 cases, incl. "no mic button ⇒ click nothing, duck nothing"). What remains for L2 is the only part L0 cannot see: that a real press on real hardware opens a real capture stream.* |
| **Touch lock** (before anything else) | Grab the node, then inject a tap via uinput; assert gamescope/Leanback observes **no** click. Release, re-tap, assert the click lands. This is the one test that decides whether the touch-lock feature exists at all. |
| **L2/R2 modifier layer** | With LT held, assert D-pad Left emits the layer's key and **not** `ArrowLeft`; released, assert the plain arrow returns. Prerequisite test: pull a trigger and assert `ABS_Z` arrives at all (input-ux §12 hazard). |
| **Touch gestures** | Inject MT events on the FTS3528 node; assert a flick produces an arrow-burst and Leanback's focus index advances past one screen. **Also assert the re-injection path:** with the node grabbed, a plain tap must still activate the focused tile via a CDP mouse click at the correct letterboxed coordinates — the behavior gamescope gives us for free today and that grabbing takes away (input-ux §11). |
| **Context keymap layers** | With player open, `B` must exit the player; inside the OSK, `B` must **not** exit search. |
| **Error/retry page** | Sever DNS; assert the launcher's own focusable retry page renders and **no** `net::ERR_*` Chromium interstitial is reachable. |
| **First-run overlay** | Fresh profile → overlay present, dismissable with a controller button, and absent on second launch. |
| **Input latency** | Log `evdev-receipt → CDP-ack` per key; assert p95 under a budget. Calibrate once against a 120/240 fps camera (L4) — the gate currently says "≤ PS5" with no method, which is unmeasurable as written. |
| **Widevine** | `navigator.requestMediaKeySystemAccess('com.widevine.alpha', …)` resolves. Must fail today (empty `CdmRegistry`) — that failing test **is** the spec for the first `patches/` entry. |
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
  `VpxVideoDecoder`, `FFmpegVideoDecoder`. Cache the latest non-null value per `playerId`.

**Do not scrape `chrome://media-internals`.** It is a WebUI over the same `MediaLog` stream with no
stable API. The `Media` domain is the supported surface for identical data.

*This also gives us a real test for the AV1 dispute (`findings/durable/hardware.md`): force an AV1
stream, and assert on `kVideoDecoderName` rather than on libva's advertised profiles.*

---

## 6. L3 — Google/YouTube conformance suites

**The public endpoints are dead. Verified 2026-07-09 by fetching them:**

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
3. **Launch or attach** to the engine (`remote-run.sh` already does a headless Game Mode launch).
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
| Launch the app in Game Mode | yes | `scripts/remote-run.sh` already launches the Flatpak headlessly |
| Start playback | `power.sh` prints "play a video now" | the cert harness plays its own media; `--autoplay-policy=no-user-gesture-required` is already set |
| Read the results table | eyeball it | `getTestResults()` scrape |
| Decide pass/fail | judgment | the committed baseline (§6.2) |
| **YouTube sign-in** | — | **not needed.** The cert harness is account-independent; it never loads `youtube.com/tv`. This is why cert automates cleanly while an end-to-end Leanback test does not. |
| **Widevine CDM install** | manual, opt-in | **stays manual, by design.** CI must never fetch Google's CDM (`docs/legal.md`). So the automated gate runs **ClearKey only**; Widevine key systems are `expected-fail` in the baseline until the P7 `AddContentDecryptionModules` patch lands. Automating this one would mean redistributing the CDM — we won't. |

### 6.5 Failure modes the harness must distinguish

Each gets a distinct exit code, because "cert failed" is useless at 3 a.m.:

| Symptom | Real cause | Guard |
|---|---|---|
| every EME test fails instantly | insecure origin | assert `isSecureContext` in preflight |
| vectors 404 | GCS bucket changed | `HEAD` + SHA-256 preflight |
| results POSTed outward | served from an `appspot`/`googleapis` host | assert `location.host == localhost:*` |
| suite never finishes | wedged test | no-progress watchdog + in-flight test name |
| video never plays | autoplay policy | assert `!video.paused` early |
| target vanishes mid-run | Leanback-style teardown | `cdp.py` already re-attaches |

### 6.6 What this gate does *not* mean

Passing `js_mse_eme` is **not** YouTube certification, confers no status, and must never appear in
user-facing material as "certified" (see §6 and `docs/legal.md`). It is a regression net built from
the same harness Google used to certify devices. That is a strong claim on its own and does not need
inflating.

## 7. CI topology

| Runner | Tiers | Trigger |
|---|---|---|
| GitHub free | L0 + lint (shellcheck, clang-format, gcc+clang launcher build) | every push/PR |
| Self-hosted (workstation, sccache) | L1 `just smoke`, both presets; L3 `just cert` (headless — no Deck) | PR + nightly |
| Self-hosted **+ Deck attached** | L2 `just test-deck`, **L3 `just cert-deck`** (adds `playbackperf-*`) | nightly + pre-release |
| Human | L4 | pre-release only |

Note the cert split: the MSE/codec/EME-ClearKey suites are *engine* behavior and run headless in the
build image, so the expensive Deck-attached runner is reserved for what genuinely needs hardware
(real GPU decode, `playbackperf-*`). A conformance gate that needed a Deck for every PR would not
survive contact with a busy week.

`just smoke` doubles as the **R1/R2 canary**: run nightly against production `youtube.com/tv` so a
server-side Leanback change that breaks the TV UA or the `ytlr-app` assertion pages us before users
find it. It also runs the §5 capability probes (`--assert-ua`, `--assert-av1-steering`), so AV1
steering breakage is caught the night it happens rather than at release. It injects
`config/av1_steering.js` — the same file the launcher compiles in — and first asserts, on the
unsteered engine, that AV1 *is* supported. Without that baseline the whole check is vacuous on any
build that lacks an AV1 decoder, which is precisely the shape of F1.

---

## 8. Anti-goals

- **No test that only proves our own code echoed our own constant.** (See §0.)
- No Selenium/Playwright/Boost-class dependency; `cdp.py` + stdlib + `pytest` only.
- No test that requires a signed-in account in CI. Sign-in is L4/manual — credentials never enter CI.
- No scraping `chrome://media-internals`; no dependence on undocumented `?env_` Leanback params
  (searched; no authoritative list exists — treat any such claim as unverified).
- Do not claim "certified." See §6.
