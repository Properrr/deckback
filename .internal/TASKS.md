# Deckback — Task Plan

Living execution checklist derived from `.internal/steamdeck-cobalt-youtube-plan.md`.
The design doc is the source of truth; this file tracks *what to do next* and *what is done*.

- **App:** Deckback — `io.github.properrr.deckback`
- **Target:** Steam Deck **OLED (Sephiroth)** — the only unit we have. **LCD (Van Gogh)**: best-effort, untested (decisions.md / R11). SteamOS 3.x, Game Mode only
- **Engine:** Chromium-based Cobalt trunk (`youtube/cobalt`, ~M114)
- **Legend:** `[ ]` todo · `[~]` in progress · `[x]` done · `[!]` blocked/needs input

---

## Phase S — Repo scaffolding (this session, no Chromium checkout required)

- [x] Repo hygiene: `.gitignore`, `.env.example`, `.clangd`, `README.md`, `.internal/TASKS.md`
- [x] `DEPS.pin` placeholder + guard (real commit set in S0.1)
- [x] GN arg presets: `args/dev.gn`, `args/deck.gn`, `args/asan.gn`
- [x] Container env: `docker/Dockerfile` (build/dev/pack stages), `docker-compose.yml`
- [x] `justfile` + `scripts/*.sh` (every recipe delegates to a script)
- [x] `config/app.json` (UA, URL, caps, keymap) + `config/steam_input.vdf` template
- [x] `launcher/` C++23 skeleton + CMake (compiles with zero external deps via `__has_include` guards)
- [x] `patches/series` (empty quilt series) + `patches/README.md`
- [x] `flatpak/io.github.properrr.deckback.yml` manifest skeleton
- [x] `.github/workflows/lint.yml` (free-runner-safe: shellcheck, clang-format, launcher build)
- [x] `docs/SUPPORT.md`, `docs/legal.md` stubs
- [x] Docker base = **Debian 12** matching Cobalt trunk (build/test/dev/pack stages, mirrored deps)
- [x] Knowledge base: `.internal/findings/` (durable vs milestone split) + `.internal/MIGRATION.md` bump playbook

**Gate:** `just --list` works; `launcher/` builds standalone; docker image builds; no Chromium tree needed.

---

## Phase 0 — Research spikes & de-risking  (4–6 ED)   → `.internal/findings/milestones/m114.md`

- [x] **S0.1** Does trunk Chrobalt build + run on generic Linux? Which Chromium milestone *now*?
      → **Pinned `DEPS.pin` = `a9181df8` (Cobalt 26.eap, Chromium M114 confirmed).** Checkout +
      hermetic toolchain fetched OK; `content_shell` **builds, links (1.4G devel), and boots headless**
      with CDP up (`just smoke`). Build uses `cobalt/build/gn.py -p chromium_linux-x64x11 -c devel` →
      `content_shell` (NOT raw gn/cobalt). depot_tools py3 bootstrap fixed in the image. **KEY:**
      Cobalt ≤25 is classic Starboard Cobalt (wrong engine); Chrobalt is 26+. See `m114.md`.
- [x] **S0.2** Which UA yields full Leanback (voice search, sign-in, settings)? Any attestation beyond UA?
      → **`Mozilla/5.0 (ChromiumStylePlatform) Cobalt/26.lts.0,gzip(gfe)` UA → interactive Leanback**
      (onboarding/browse, `ytlr-app`, `youtube.com/tv#/`, no desktop redirect); seeded into
      `config/app.json`. This is the verified TV-client form: desktop/SteamDeck tokens select a
      different style bundle and render overlay text with opaque black backgrounds. NOTE: a `CrKey`
      (Chromecast) UA loads `ytlr-app` but drops into **cast-receiver "Ready to cast"** mode —
      unusable; smoke asserts not-cast AND not-desktop. **content_shell ignores `--user-agent`**
      (Cobalt hardcodes Chrome/999) → UA injected via CDP; launcher needs a patch or startup CDP
      override (R1, Phase 2/3). Full sign-in/voice are on-Deck checks. See `m114.md`.
- [x] **S0.3** Is Chromium library-CDM path (`enable_library_cdms`/`enable_widevine`) intact or stripped?
      → **INTACT** (not stripped). `third_party/widevine/cdm/` present; `widevine.gni` gates it behind
      `enable_widevine` (default false). Re-enable via GN args + user-fetch CDM (gates Phase 7).
- [x] **S0.4** Ozone: does trunk support `--ozone-platform=x11`? Wayland? What under gamescope?
      → **YES.** deck build renders **interactive Leanback on the Deck** via `--ozone-platform=x11` on
      gamescope's Xwayland `DISPLAY=:1`, real RADV gpu-process (no SwiftShader). Confirmed by CDP
      screenshot (onboarding UI). Still TODO on-device: video playback (S0.5), gamepad (S0.6). See `m114.md`.
- [x] **S0.5** Media stack: Chromium media pipeline (FFmpeg/mojo) or Starboard `SbPlayer` on Linux?
      → **Chromium media pipeline** (`RendererImpl`); **zero** `SbPlayer`/starboard symbols in the
      binary. **VA-API HW decode WORKS** with exactly `--enable-features=VaapiVideoDecodeLinuxGL
      --use-angle=gl` (verified: `kVideoDecoderName=VaapiVideoDecoder`, `kIsPlatformVideoDecoder=true`,
      VP9 1280×720, 0/84 frames dropped). `--use-angle=gl` is **required** — the default ANGLE/Vulkan
      backend silently falls back to software. Ignore-blocklist flags are NOT needed.
      **Today we ship software decode**, and YouTube serves us AV1 → `Dav1dVideoDecoder` on the CPU.
      ⚠ **AV1 hardware decode measured PRESENT** on the OLED (libva `AV1Profile0`/VLD) — disputes
      `durable/hardware.md`. See `m114.md` §"Media stack (S0.5)". Gate for Phase 4 is green.
- [x] **S0.6** Gamepad: does Blink's Gamepad API see evdev pads? Does Leanback react under console UA?
      → **Leanback is fully key-navigable**; CDP `Input.dispatchKeyEvent` events are **trusted**
      (`isTrusted=true`) so Leanback's real handlers fire. Mechanism = **(b′) launcher-side evdev→key
      over the existing CDP bridge — NO Cobalt patch** (`patches/series` stays empty; keymap can live
      in `config/app.json`, a plan design goal). Under Steam Input the readable pad is the virtual
      `Microsoft X-Box 360 pad`, not the raw Deck controller (only it advertises `BTN_SOUTH`).
      **On-Deck gamepad exercise DONE 2026-07-08** — physical presses and a synthetic uinput pad both
      drive Leanback focus end-to-end. See `m114.md` "Input on-Deck verification".

**Gate:** `.internal/findings/milestones/m114.md` filled in; go/no-go on plan adjustments; `DEPS.pin` pinned.

---

## Phase 1 — Build & bring-up on Deck  (5–7 ED)

- [ ] Reproducible build: pin trunk commit in `DEPS.pin`; `just gen dev/deck` + sccache verified.
- [ ] First run on Deck: Desktop Mode (X11) then Game Mode as non-Steam shortcut.
- [ ] Force 1280×720 render surface; verify text sharpness vs. 1080p-authored Leanback.
- [x] Persistent profile dir (`~/.var/app/...` shape) so sign-in survives updates.
      **DONE in code** (`main.cpp:resolve_profile_dir`): default is now `$XDG_DATA_HOME/deckback/profile`
      (Flatpak points that at the durable `~/.var/app/<id>/data`), then `~/.local/share/...`; the
      ephemeral `XDG_RUNTIME_DIR` is a last resort only (it was the previous default — a tmpfs wiped on
      logout, so sign-in would not have survived a reboot). `$DECKBACK_PROFILE` still overrides.
      **Correction 2026-07-09 (review):** the dir was being passed as `--user-data-dir`, a
      chrome/-layer switch content_shell/cobalt-shell silently ignore — the engine's switch is
      **`--data-path`** (`shell_switches.cc:kContentShellDataPath`). Fixed in `main.cpp` +
      `smoke.sh`; earlier runs actually kept state in `$XDG_CONFIG_HOME/content_shell`.
      **On-Deck VERIFIED 2026-07-09:** sign-in survived the rebuilt Flatpak reinstall and Steam
      Game Mode relaunch. Startup migration now copies a non-empty legacy runtime/config profile
      into the durable path through staged atomic promotion, without overwriting an existing
      durable profile (`launcher/src/profile.cpp`, `profile_test`).
- [ ] Crashpad on, local minidumps only (no telemetry).

**Gate:** cold boot → Game Mode → launch → browse (touch) → play 1080p+audio, stable 30 min.

## Phase 2 — Session & lifecycle plumbing  (3–4 ED)   → `launcher/`

- [~] Launcher owns env/flags/config/log-rotation/watchdog restart-on-crash.
      **log-rotation DONE** (`launcher/src/log.cpp`: size-capped rotating file sink + stderr mirror,
      config-driven, thread-safe; `log_test`). env/flags/config/watchdog already in place.
- [~] Single-instance lock; deep-link support (`app://` → `youtube.com/tv#?v=`).
      Both implemented in `main.cpp` (flock guard + `apply_deep_link`). **Deep link on-Deck VERIFIED
      2026-07-08**: `app://<id>` lands on `#/watch?v=<id>` with `readyState=4`. Caveat — on a fresh
      profile Leanback's account gate ("Watch as guest") swallows every route until dismissed, which
      mimics a broken deep link. Single-instance lock still unverified on-Deck.
- [~] Clean exit: controller chord + Leanback Exit → proper shutdown; Steam "Close game" safe.
      **SIGTERM/"Close game" now exits 0** (`watchdog.cpp`; `watchdog_test`). Controller-chord exit
      is Phase 3 (input layer).
- [x] **Failure-state UX. DONE 2026-07-09** (findings input-ux §16). `launcher/src/errorpage.{hpp,cpp}`
      + `DevToolsClient::navigate_checked()`. On a failed navigation the Navigator goes to
      `about:blank` and injects a controller-focusable Retry page (Enter/Space; Escape deliberately
      unbound), retrying with 2s→30s backoff; a user-requested retry resets the backoff.
      `error_title`/`error_hint` in `app.json` are the R1 hotfix surface.
      **This fixed a silent bug**: `Page.navigate` reports failure as a *successful* command with an
      `errorText` field, and the failed document keeps `location.href` set to the URL we asked for.
      The old liveness check read that href and concluded the TV app had loaded, so a network outage
      parked the user on Chromium's desktop interstitial forever, unlogged.
      **⚠ Unverified**: that a CDP-injected document renders on `about:blank` under gamescope, and
      that our injected Enter reaches our own button. Both are L2 tests: pull the network, assert
      focus.

**Gate:** 50 launch/kill cycles via Steam UI, zero profile corruption. *(on-Deck; not yet run)*

## Phase 3 — Input: PS5-parity controls  (6–8 ED)   → `launcher/src/input.cpp`, `config/steam_input.vdf`

- [x] Mechanism from S0.6: **(b′) launcher-side evdev→key over CDP**, not the planned in-tree
      `ui::KeyEvent` patch. `launcher/src/input.{hpp,cpp}` (`GamepadInput`), wired in `main.cpp`
      behind `cdp_nav`. Reads raw `<linux/input.h>` (no libevdev), opens every gamepad-capable
      device and merges. **Verified on-Deck 2026-07-08** against the deployed `deck` build: real
      controller presses AND a synthetic uinput pad both move Leanback focus and activate (A → the
      Terms & Privacy overlay opened). Multi-device merge confirmed (Steam auto-wraps an attached pad
      into a second Xbox-360 node; all opened, no duplicate keys). **Also confirmed in Game Mode via
      the Steam shortcut** (flatpak) after rebuilding it — the installed bundle predated the input
      commit, so `just deploy` alone leaves Game Mode stale. See `m114.md` "Game Mode (flatpak)".
- [x] Default mapping now comes from **`config/app.json:keymap`**, not from hardcoded C++.
      `build_button_map()` resolves each value either as a DOM key (`Enter`, `MediaPlayPause`, `c`)
      dispatched verbatim, or as a semantic alias (`select`, `back`, `playpause`, `toggle_captions`,
      `seek_back_10`, `seek_fwd_10`). This keeps the hot-swap promise in doc §1: a Leanback change
      can be worked around by editing config, with no rebuild. Auto-repeat + acceleration retained
      (acceleration still not exercised on-device). Bound today: A→Enter, B→Escape, X→MediaPlayPause,
      LB→ArrowLeft, RB→ArrowRight, Select→c; D-pad + left stick → Arrows.
      **Deliberate non-guessing:** `voice_search`, `player_menu`, `scan_rewind`, `scan_forward` have
      no established Leanback key, so they resolve to nothing and are named in a startup `WARN`
      rather than silently bound to a plausible-looking key.
- [x] **Hotplug gap fixed.** `rescan_devices()` still handles device-lost; a new
      `discover_new_devices()` runs every `kHotplugScanMs` (2 s) and opens only paths not already in
      `paths_`, so an already-open pad is never torn down. **Verified on-Deck 2026-07-09:** with
      Steam's `event10` open, a uinput pad created *after* startup was adopted (`event11`, plus the
      second Xbox node Steam spawned in response) and its A press dispatched `btn 304 -> Enter`.
- [x] **Analog triggers** (LT/RT, `ABS_Z`/`ABS_RZ`) get press/release hysteresis
      (`trigger_pressed()`: press ≥192, release <64) so a trigger resting mid-travel cannot chatter.
      **They are dead buttons as shipped**: `lt`/`rt` → `scan_rewind`/`scan_forward` → no DOM key →
      nothing dispatched. See the L2/R2 modifier task below — this makes them free to repurpose.
- [ ] **⚠ Trigger binding hazard (verify before relying on L2/R2).** `config/steam_input.vdf` binds
      the triggers as `"click" → xinput_button TRIGGER_LEFT/RIGHT` (a **digital** press), but
      `input.cpp` reads the **analog axes** `ABS_Z`/`ABS_RZ`. Whether Steam's virtual pad still emits
      the analog axis under that binding is **unverified**. If it emits a digital button instead, the
      hysteresis path never runs and the triggers are dead for a second, unrelated reason. One-line
      check on-Deck: `evtest` the virtual pad and pull a trigger.
- [~] **Runtime touchscreen block/unblock** (findings input-ux §4, the explicit requirement).
      **DONE in code** — `launcher/src/touch.{hpp,cpp}` `TouchGuard`: RAII `EVIOCGRAB` on the FTS3528
      (resolve by USB id `2808:1015` + MT-capability/name; node re-resolved on every grab so it
      survives resume renumbering). Initial state from `app.json:block_touchscreen`; a controller
      chord (`touch_lock_chord`, default `l3+r3` — both stick clicks) toggles the lock at runtime via
      `GamepadInput::handle_chord`; the grab is always released on exit; **the controller is never
      disabled**. Needs Flatpak `--device=input` (granted at install). `parse_chord` + config touch
      parsing unit-tested.
      **Feedback DONE** (findings input-ux §14): engaging the lock now shows a CDP-injected toast
      naming the chord, pulses the pad (`EVIOCSFF`/`FF_RUMBLE`, its own `O_RDWR` fd), and
      **unlocking requires holding the chord for `touch_lock_unlock_hold_ms` (800 ms)** while locking
      stays immediate — two stick clicks are too easy to trip for an accidental unlock to be
      acceptable. `TouchLockChord` + `toast_js` + `make_rumble_effect` unit-tested and
      mutation-checked; `overlay_test` is the new target.
      **⚠ The fd is grabbed but never read** (`touch.cpp` opens `O_RDONLY`, then only `EVIOCGRAB`s).
      So the lock is total, and **§4's remaining policy is structurally unimplementable as built** —
      no deliberate-center-tap escape hatch, no palm rejection. Both need the grab+read path priced
      out in input-ux §11.
      **⚠ SUPERSEDED 2026-07-10 — the load-bearing assumption was proven FALSE on a Deck.** gamescope
      reads the panel *without* an exclusive grab, so `EVIOCGRAB` does NOT starve it — touch flowed
      straight through the grab. The lock is a no-op (it would have announced a lock that never
      engaged). Touch is instead made inert by `disable_touch` (page swallow + gamescope hover-lock),
      verified 2026-07-10; the dead grab machinery ships disabled. See durable/touch-lock.md.
- [ ] Leanback OSK usable end-to-end. **Steam OSK is NOT a safe fallback** (input-ux §8.3):
      `STEAM+X` on a non-Steam Chromium shortcut can raise the *desktop* OSK and soft-lock the
      session → the reliable path is **QAM ("…") → keyboard**; and under `--ozone-platform=x11` the
      OSK **never auto-appears on `<input>` focus** (needs Wayland `text-input-v3`, which we don't
      have, and we can't call the Steamworks OSK API). *Spike (input-ux §8.4): what do
      Escape/Backspace/Delete do INSIDE the OSK? Strong prior from `starboard/key.h`: `kSbKeyBack ==
      kSbKeyBackspace == 8`, so **B/Backspace likely pops the view rather than deleting** —
      `Delete`(46) is the candidate erase key. Needs the context-layer task below.*
- [~] Ship `config/steam_input.vdf` + import instructions; optional focus-move rumble (off by default).
      **Template expanded** to full groups (face/dpad/sticks/triggers/switches/trackpads/gyro),
      `controller_neptune`, **rear grips L4/L5/R4/R5 duplicating face buttons** for reachability,
      right trackpad as an optional pointer (inactive by default), gyro off. Standard controls pass
      through to the virtual pad so `input.cpp` reads them. *Byte-authoritative binding must be
      exported from Steam's configurator on-Deck (Phase 3 gate); grid art + rumble still TODO.*
- [x] **`launcher/tests/input_test.cpp`** covers the pure logic: binding resolution (DOM keys,
      aliases, and the refusal to guess), direction resolution (diagonal→vertical, D-pad beats stick,
      stable pointer identity), button-map construction against the shipped keymap, trigger
      hysteresis, and `Config::load` keymap parsing. `just launcher test` → **8/8** (config, log,
      devtools, player, watchdog, sha256, profile, input; re-checked 2026-07-09).
- [x] `deckback-launcher --help` now prints usage and exits 0. It previously fell through the
      unknown-arg branch and **launched YouTube**, as did any typo'd flag; unknown args now exit 2.

*UX-gap tasks from the 2026-07-09 review (input-ux §7):*

- [ ] **Touch-lock feedback + deliberate unlock** (input-ux §4 policy, currently violated — the
      chord toggle is silent, log-only). Engage = lock glyph/toast + haptic; unlock = deliberate
      hold or two-step so a stray chord can't defeat it. Overlay channel: CDP-injected DOM toast
      (no engine patch); haptic via FF (`EVIOCSFF`) on the virtual pad — no SDL dep needed.
- [ ] **Bind the researched keys** — *rewritten 2026-07-09 after input-ux §8.1 corrected §2.*
      `j`/`l`/`k`/`/`/`Shift+N/P` are **desktop watch-page** shortcuts, not a TV-app contract, and
      the CEA codes (461/415/412/417) belong to Tizen/webOS, **not Chromium/Cobalt**. Bind nothing
      without the §8.4 spike.
- [x] **Seek-model contradiction resolved in docs** (input-ux §12). Three artifacts disagreed:
      SUPPORT.md promised "L1/R1 seek ±10 s", `app.json` says `seek_back_10`, and `input.cpp` maps
      that alias to `ArrowLeft` — the console **scrub** model. **SUPPORT.md corrected 2026-07-09** to
      describe scrubbing and to state plainly that no fixed-interval seek key exists on the TV
      surface. *No mechanism can deliver ±10 s today — the blocker is the missing action key, not the
      input device.*
- [x] **Renamed the misleading aliases** `seek_back_10`/`seek_fwd_10` → `scrub_back`/`scrub_fwd`
      (they dispatch arrows, not a 10 s jump). **DONE 2026-07-09**: `input.cpp:kActionAliases` gained a
      `replaced_by` column; the old names still resolve (a remotely hot-swapped `app.json` must not
      break) but emit a startup `WARN` via the new pure `deprecated_action_replacement()`.
      `config/app.json` now uses the new names; SUPPORT.md was already correct. A new `input_test`
      case loads **the real shipped `config/app.json`** (`DECKBACK_APP_JSON` from CMake) and asserts it
      uses no deprecated alias, so config can no longer drift from the C++. Add a real `seek_back_10`
      only if spike §8.4 #3 finds a key that actually seeks.
- [x] **`devtools.cpp` dispatch surface widened. DONE 2026-07-09.** The 12-entry `kKeys` table is
      replaced by `key_spec()`, which resolves either a named key or **any single printable ASCII
      character** (letters/digits/space/punctuation, incl. shifted forms carrying the physical key's
      `code`, e.g. `'?'` → `Slash`). Added `MediaRewind`(227)/`MediaFastForward`(228) (tree-verified in
      `cobalt/src/starboard/key.h`: `0xE3`/`0xE4`), `Delete`(46) and `Tab`(9). Printable keys dispatch
      `keyDown`+`text` (not `rawKeyDown`), which is what makes `beforeinput`/`textInput` fire — the
      prerequisite for text entry. Added the `modifiers` bitfield (`mod::kAlt/kCtrl/kMeta/kShift`) and
      **`Input.dispatchMouseEvent`** (`mouse_press`/`mouse_release`/`mouse_click`) for voice V1.
      `devtools_test` asserts the **wire format** against a request-capturing fake CDP server, not our
      return values; both the VK codes and the `text` field were mutation-tested.
      *Nothing new is bound:* `scan_rewind`/`scan_forward` remain unmapped until the on-Deck spike.
- [x] **Re-scoped `y = voice_search`: it is not a key. DONE 2026-07-09** (input-ux §8.2). The input
      layer no longer treats it as a binding at all: `find_voice_control()` resolves it to an evdev
      code that `handle_voice()` intercepts, and `without_voice_control()` removes it from the button
      map **only when voice is enabled** — with voice off it stays in the startup "unmapped" warning,
      because then it really is a dead control. Activation is a trusted `Input.dispatchMouseEvent` on
      the soft-mic button's rect centre, not focus+Enter (more robust, and no letterbox transform).
- [x] **Right stick: fast list traversal. DONE 2026-07-09** (findings input-ux §15). `fast_scroll()`
      in `input.cpp` (pure, unit-tested, mutation-checked) maps the raw `ABS_RX`/`ABS_RY` deflection
      to an arrow key plus a **repeat rate**: `right_stick_slow_ms` (200) at the deadzone edge,
      linear to `right_stick_fast_ms` (45) at full travel, re-read from the current deflection on
      every step. The rate is analog *on purpose* — the D-pad accelerates over time because it is
      digital and cannot know how hard you push; a time ramp here would fight the thumb for the same
      variable. Dominant axis wins, vertical takes the tie. Suppressed while the D-pad/left stick
      hold a direction. Routed through `resolve_direction_key()`, so layers rebind it and **no new
      key assumption is introduced** — arrows are verified.
      **⚠ Unverified**: that Steam's virtual pad emits `ABS_RX`/`ABS_RY` under our `steam_input.vdf`
      at all (same hazard class as the L2/R2 trigger axes above), and that Leanback tolerates a 45 ms
      arrow cadence. Both fail safe: no axis = no scroll; dropped keys = slower scroll.
- [x] **Context-aware keymap layers + L2/R2 held-modifier layer. Mechanism DONE 2026-07-09**
      (one change: they are the same machinery). New `launcher/src/layers.{hpp,cpp}` holds the
      `Layer{Browse,Player,Osk}` enum, `resolve_layer()`, and a `LayerState` (relaxed atomic) written
      by `PlayerController`'s poll thread and read by `GamepadInput`'s.
      `player.cpp` now fetches **three signals in one CDP round trip** as a bitmask (`playing`,
      `player_open`, `text_input_focused`) — three separate evaluates per tick would triple poll cost
      and could observe an inconsistent page. `player_open` keys off the `#/watch` hash route, whose
      shape is *verified* by the 2026-07-08 deep-link test. An unreachable engine decodes to all-false
      → `Browse` → today's context-free behavior, so a crash mid-playback can never strand the input
      on player bindings (`player_test` covers this).
      `input.cpp` gained `KeyLayer`/`Keymaps`/`build_layer`/`resolve_button`/`resolve_direction_key`.
      **The two layer kinds differ on purpose:** a *context* layer (player/osk) only overrides — an
      unbound control falls through to base; a *modifier* layer (held LT/RT) **absorbs** an unbound
      control, because falling through would make LT+A fire a plain A while the user holds a modifier
      precisely to mean "not the normal action". An empty modifier layer is not a modifier at all, so
      the trigger keeps its own binding. LT wins over RT when both are held. Directions are rebindable
      per-layer and re-resolved on **every auto-repeat**, so releasing a modifier mid-hold takes effect
      without re-pressing. Config: `keymap_player`/`keymap_osk`/`keymap_lt`/`keymap_rt`, all
      hot-swappable, **all shipped EMPTY** — the mechanism exists, no key is guessed. `input_test`
      covers precedence/absorb/fall-through and asserts the shipped `app.json` layers stay empty.
      Still blocked for *binding* anything: the trigger binding hazard above (does the virtual pad even
      emit `ABS_Z`?) and the §8.4 key spikes.
- [ ] **Touch gesture layer** (input-ux §3 "richer touch layer" + §7 — beats the Switch YT app,
      which shipped without touch scrolling): read FTS3528 MT events in the launcher and translate —
      flick → arrow-key bursts (**Leanback has NO touch scrolling today**; rails are untraversable
      by touch), left-edge swipe → Escape (**no touch Back path exists**), double-tap L/R → seek,
      tap-hold → 2× (pending the speed spike).
      **⚠ Price this before committing (input-ux §11).** `EVIOCGRAB` is exclusive — there is no
      "observe without stealing". Reading touch without grabbing gives **double actuation** (our
      gesture *and* gamescope's click), so a gesture layer **forces the permanent-grab path**. Then:
      tap-to-activate stops being free and becomes ours to re-inject as CDP mouse clicks (including
      the 720p→800p letterbox coordinate transform); palm rejection becomes ours; and a launcher
      crash kills touch until the kernel releases the grab. The same grab+read path is what finally
      makes §4's escape hatch and soft-suppress possible — **lock and gestures are one mechanism and
      must be designed together.** This is an architectural commitment, not an incremental feature.
- [ ] **On-Deck key spikes** (no-guessing policy — bind only after verification; probes + the
      capturing listener are written out in input-ux §8.4 and TEST-PLAN §4). Every spike ships with
      **negative controls** (inject CEA `461`/`415` and require they do nothing), else "the key
      worked" is unfalsifiable. Covers: OSK erase semantics (8 vs 27 vs 46); **direct printable
      typing** into the search field (the highest-value one — it decides whether a real/BT keyboard
      can ever type into Leanback, or the grid OSK is D-pad-only); transport keys 179/178/227/228/
      176/177 + colors 403–406; soft-mic reachability; speed/chapters (no TV hotkey is documented —
      speed is a D-pad menu).
- [ ] **Text-entry story, end to end** (new workstream — input-ux §8). Decide and document the
      supported path: Leanback grid OSK via D-pad (always works), QAM keyboard (never `STEAM+X`),
      physical/BT keyboard (**reaches the app, but Game Mode forces QWERTY scancodes regardless of
      the user's layout**), and voice via the soft-mic button. The launcher currently has **no text
      path at all**: `input.cpp:is_gamepad()` only opens devices advertising `BTN_SOUTH`, so a
      keyboard is never read, and `devtools.cpp` cannot dispatch arbitrary characters.
- [ ] **Steam Input keyboard-emission fallback** (input-ux §8.3): `config/steam_input.vdf` is pure
      xinput passthrough today. Steam Input can bind controller inputs to **arbitrary keyboard keys**
      (by scancode) even for non-Steam shortcuts — a live fallback if CDP injection ever fails, and
      the per-user remap layer. Evaluate; do not adopt blindly (scancode≠layout).
- [ ] **Focus-trap sweep** (Deck-Verified gate, input-ux §9): for every surface, press every
      direction from every focusable element; assert focus always moves or is deliberately absorbed,
      and nothing becomes unreachable. This is the most common TV-app defect and we have never
      tested it.
- [ ] **Smallest-glyph measurement** (Deck-Verified hard number): smallest rendered character must be
      ≥ **9 px @1280×800** (12 px recommended). Leanback's secondary metadata text is the risk, and
      we render from a 720p buffer. Measure on-device from a screenshot.
- [ ] **Back-at-root semantics** (input-ux §9, handheld inversion): repeated Back on Android TV lands
      on the OS launcher; in our kiosk it must land in **Steam Game Mode**, never a blank shell or
      Chromium chrome. Decide deliberately: swallow at the Leanback root, or clean-quit.
- [ ] **Input latency measurement** (the gate says "≤ PS5" with no method): log the evdev-receipt →
      CDP-ack delta per key in the launcher, calibrate once against a 120/240fps camera on-Deck.
- [ ] **Auto-repeat acceleration tuning** on-Deck (Verified bar: fast but not overshooting; the
      acceleration code exists but has never been exercised on-device).
- [x] **First-run controls overlay. DONE 2026-07-09** (findings input-ux §17).
      `launcher/src/onboarding.{hpp,cpp}` + `onboarding_test`. A modal CDP-injected card, drawn once
      the TV app has loaded (`Navigator::set_on_app_loaded`), dismissed by any *button* (never a
      stick — it drifts at rest), re-openable via the `show_controls` action.
      **The rows are derived from the live `app.json`, never hardcoded**: rebind a control and the
      card says so; a control resolving to no DOM key (`L2`/`R2` today) is omitted rather than
      advertised as dead; `voice_search` appears only when voice is enabled. A test reads the real
      shipped config and asserts no row is a dead promise.
      `start` moved from `player_menu` (a verified no-op) to `show_controls`. Marker is written on
      *show*, versioned (`first_run_v1`), in `$XDG_STATE_HOME/deckback/`.
      **⚠ Unverified**: that the card renders over Leanback, and that swallowing evdev events really
      freezes focus behind it. Both are L2: show card, press D-pad, assert `activeElement` unmoved.

**Gate:** every Leanback surface controller-drivable; latency ≤ PS5 app; OSK works.
*(buttons + hotplug verified on-Deck 2026-07-09; OSK and latency still unmeasured)*

**Open caveat:** `toggle_captions` (Select→`c`) dispatches a trusted `c` keydown with the correct DOM
`code=KeyC` — confirmed by a page-side key recorder — but could **not** be functionally verified: the
test video `aqz-KE-bpKQ` reports `textTracks: 0`. Re-test on a video that actually has captions.

## Phase 4 — Media pipeline & hardware decode  (8–12 ED)   → `config/app.json` (S0.5 UNGATED)

S0.5 answered the two big unknowns: the pipeline is Chromium's (not `SbPlayer`), and VA-API decode
works. Note the mechanism is **flags in `config/app.json:cobalt_flags`, not a `patches/` quilt change**.

- [✗] Step 3 (attempted, then REVERTED 2026-07-10): adding
      `--enable-features=VaapiVideoDecodeLinuxGL` + `--use-angle=gl` selected `VaapiVideoDecoder`
      (`kIsPlatformVideoDecoder=true`) BUT rendered **green-band corruption on every frame** —
      ANGLE's DMA-buf import of the tiled NV12 VA surface is broken on radeonsi (the exact failure
      the "zero-copy SETTLED" item below predicted). A 7-variant on-Deck bisection proved the
      corruption is inseparable from `VaapiVideoDecoder`; every clean config had silently fallen back
      to software `VpxVideoDecoder`. **Flags removed from `config/app.json`; we ship software decode.**
      A user reported the green lines — every automated metric (decoder name, `corruptedVideoFrames=0`)
      false-passed. See `m114.md` §"VA-API decode is VISUALLY CORRUPT" + `durable/hardware.md`.
      **P4 hardware decode is BLOCKED, not done.** Re-enable requires fixing the ANGLE↔VA-API import
      at the engine/Mesa layer (or a working gamescope hardware-overlay path), gated on a PIXEL check.
- [ ] Step 1: VP9 1080p60 headroom + baseline battery draw, sw vs hw (`just power`).
- [~] Step 2: `MediaCapabilities`/`canPlayType` steering. **DONE in the launcher, patch-free**
      (`navigator.cpp` injects a CDP `Page.addScriptToEvaluateOnNewDocument` script overriding
      `MediaSource.isTypeSupported`, `HTMLMediaElement.canPlayType`, and
      `navigator.mediaCapabilities.decodingInfo` so **AV1 reads unsupported while VP9/H.264 pass
      through**). Sticky across Leanback's target teardown; gated by `quality.steer_av1_unsupported`.
      **JS logic verified offline** (node/vm: `av01`/`av1` → unsupported everywhere, `vp9`/`vp09`/`avc1`
      → supported, `avc1` not mis-caught). This is the "MediaCapabilities patch" CLAUDE.md/common.gn
      referenced — realized in the launcher instead of `patches/`. *On-Deck: confirm YT now serves VP9,
      `kVideoDecoderName` no longer `Dav1dVideoDecoder`.*
- [ ] **Close the AV1 dispute** (blocks any decision to *allow* AV1): force an AV1 stream and confirm
      `kVideoDecoderName=VaapiVideoDecoder`; repeat the libva profile query on an **LCD** unit.
      `vainfo` is absent on SteamOS — query libva via ctypes (recipe in `m114.md`).
- [ ] Step 4: power validation — target ≤ ~8–9 W for 1080p VP9 (vs ~12–15 W sw).

*Platform-optimization tasks from the 2026-07-09 deep-research pass (`durable/platform.md` +
m114.md §"Platform-optimization research"):*

- [ ] **Sweep the QAM knobs in the power matrix** (`just power` runs): OLED refresh 90→60→45 Hz,
      per-game framerate cap on/off, TDP-limit sweep during 1080p VP9 playback. These are the ONLY
      per-app compositor/power levers available in Game Mode (gamescope's flags are Valve's; the
      `-r`/`-o` per-app-cap claim was refuted). Winning combo feeds the P8 SUPPORT.md task.
- [ ] **Direct-scanout check** (on-Deck): confirm gamescope direct-flips our fullscreen 1280×800
      Xwayland surface during playback (gamescope debug HUD / gamescopectl); if it composites, find
      what disqualifies us. Keep the window native 800p — never make gamescope scale us.
- [ ] **Audio-wakeup experiment** inside `just power`: `--audio-buffer-size=<frames>` +
      `PULSE_LATENCY_MSEC` (PipeWire quantum via the pulse-compat path) — measure the draw delta,
      confirm A/V sync unharmed.
- [ ] **Memory: trial `--process-per-site`** (single-origin kiosk; switch verified in M114) —
      measure RSS / renderer count under a long browse+play session; zram (~half RAM, SteamOS
      3.6+) is the pressure backstop, not a budget.
- [x] **Zero-copy question SETTLED — tolerate one copy** (resolves the plan §6 P4 step 3 hedge):
      M114 has no zero-copy-GL feature at all, and radeonsi's disjoint dma-buf plane export breaks
      Chromium's zero-copy import on AMD anyway. No work item remains; do not chase it.

**Gate:** 4 h continuous battery playback (OLED), no thermal throttling, no A/V desync after seek storms.

## Phase 5 — Audio & microphone  (3–5 ED)

- [x] Audio out via PipeWire; speaker playback path verified on-Deck. SteamOS denies mute changes
      from inside the Flatpak (`Access denied`), so `scripts/install-audio-repair.sh` installs a
      host-side user service scoped to Deckback's sink input. **On-Deck VERIFIED 2026-07-09:** a
      live muted stream recovered to `Mute: no`. 3.5 mm / BT hot-swap remains unverified.
- [~] Mic in `getUserMedia` → PipeWire; auto-grant mic for youtube.com origin only (embedder policy).
      **Auto-grant DONE** (`navigator.cpp` → `DevToolsClient::grant_permissions` =
      `Browser.grantPermissions` `audioCapture` scoped to the app origin only; sticky across reconnect;
      gated by `mic_autogrant`). PipeWire routing of the captured stream is runtime/Flatpak (pulse
      socket already in finish-args). *On-Deck: confirm no mic prompt + a live capture stream.*
- [ ] Voice search e2e on the OLED mic array + BT headset mic (no LCD unit — decisions.md).
      **Design + spike ladder: findings input-ux §13.** It is *not* a keypress — see below.

*Voice input (hold-to-talk), designed 2026-07-09 (input-ux §13). Stop at the first spike that fails.*

- [ ] **V0 — spike before binding anything: does our own UA hide the mic button?** We ship a Cobalt
      UA, so Leanback may assume the platform supplies voice via `h5vcc`/`SoftMicPlatformService` and
      either not render a web soft-mic button, or render one whose handler calls a missing API.
      **Our identity bet (R1) could be the thing that breaks voice.** Probe `typeof window.h5vcc` and
      query the DOM for a mic control. A patch-free hypothesis if it is gated: shim the service via
      `Page.addScriptToEvaluateOnNewDocument` (the technique `navigator.cpp` already uses for AV1
      steering), advertising `micGesture:"HOLD"` — **unverified, the protocol is Cobalt-internal.**
      If there is no button and no working shim, **voice is not deliverable on this engine; say so
      rather than ship a dead button.**
- [x] **V1 — `Input.dispatchMouseEvent` added to `devtools.cpp`. DONE 2026-07-09**
      (`mouse_press`/`mouse_release`/`mouse_click`; press and release are separable so hold-to-talk can
      hold the button down).
- [x] **V2 — hold-to-talk. DONE 2026-07-09.** New `launcher/src/voice.{hpp,cpp}`: a pure,
      clock-injected `HoldToTalk` state machine (press → pending, tick past `voice_hold_ms` → Start,
      release → Stop) wired into `input.cpp` via `handle_voice()`, which intercepts the voice control's
      press/release edges before the press-edge-only button path. The poll loop now wakes at the hold
      deadline — a held button emits no further evdev events, so otherwise the mic would open on the
      next unrelated event, or never. A kernel auto-repeat or a duplicate press from Steam's merged
      second pad does **not** reset the deadline. `voice_click_toggles` covers the tap-to-toggle page.
- [x] **V3 — duck/pause while capturing. DONE 2026-07-09** (in `VoiceController`, not
      `PlayerController` — it owns the mic lifetime). `voice_duck` = `none|mute|pause`, default
      **pause**, applied *before* the mic opens because the first word is the one ASR needs most.
      Restored on stop, and only when we were the ones who changed it. `input.cpp` also stops voice on
      exit so the mic is never left open and playback never left ducked. **A voice spike run in a
      silent room still has not tested the real case** — the gate requires a video playing.
- [ ] **V4 — button choice.** Default `y` (already nominally `voice_search`, unbound, and where TV
      apps put search). To put it on a rear grip instead: grips **are not on the virtual pad** and
      evdev cannot read them (§1). The only route is a Steam Input binding mapping a grip to a
      standard xinput button — and `steam_input.vdf` maps all four grips onto face buttons today, so
      a grip is indistinguishable from the face button it duplicates. Free one first (e.g. `START`,
      whose `player_menu` is dead anyway).
- [ ] **V5 — prove the P5 auto-grant on hardware**: activating the mic must call `getUserMedia` with
      **no permission prompt**, and the captured track must be **live and non-silent** through the
      Flatpak pulse socket. Assert on audio levels, not on the absence of an error.

**Gate:** voice search succeeds in a normal room on the **OLED**, *with a video playing* (V3), and
with a Bluetooth headset mic; audio hot-swap without restart.

## Phase 6 — Power: sleep, resume, idle inhibit  (4–6 ED)   → `launcher/`

- [x] Suspend: logind `PrepareForSleep` delay-inhibitor → pause, checkpoint, flush → release.
      **DONE.** sd-bus mechanism (`platform.cpp`) + the pause/checkpoint hook now real via the
      DevTools bridge: `PlayerController::on_suspend` pauses `<video>` and logs its position while the
      delay inhibitor is held (`launcher/src/player.cpp`, `player_test`).
- [~] Resume: wait network-online (NM/probe backoff), rebuild PipeWire, nudge player, refresh token.
      **Nudge + network-online wait + token refresh DONE.** `on_resume` waits on `wait_online()`
      (netprobe TCP backoff to `resume_probe_*`) before acting, then nudges `play()`. **Token/stream
      refresh added**: if the Deck slept ≥ `resume_reload_after_ms` (CLOCK_BOOTTIME delta; 0 = off by
      default), it `Page.reload`s so Leanback re-fetches fresh stream URLs + auth instead of nudging a
      dead `<video>` (`player_test` covers both the reload and the disabled path). PipeWire rebuild is
      runtime/hardware (out of launcher scope). *On-Deck: tune the threshold via `just soak`.*
- [x] Idle-inhibit while playing (ScreenSaver/logind); fallback synthetic activity behind config flag.
      **DONE.** logind block inhibitor (`set_idle_inhibited`) is now driven by a real play-state
      source: `PlayerController` polls `document.querySelector('video')` state over DevTools every
      `devtools_poll_ms`. Synthetic-activity fallback is plumbed behind a config flag (warns; not yet
      implemented — logind path is primary).

- [x] **Measure the post-resume EPP penalty** in `just soak` (verified: SteamOS 3.8.2-beta resets
      EPP on cores 1–7 from `balance_performance` to `performance` after resume → higher draw/heat,
      [steamos#2383](https://github.com/valvesoftware/steamos/issues/2383)). **BUILT 2026-07-09; RAN
      2026-07-10 in the soak — EPP was unchanged across resume on the tested build (T5); the reset is
      a version-specific hazard, still reported and never gated.** `deckctl.py epp` reads every core
      before the first suspend and after each resume;
      `soak.sh` counts the cycles on which it moved and names the transition. Root-only sysfs → **we
      cannot fix it from the Flatpak**, so it is reported and never gated. Still to do: document the
      affected builds in SUPPORT.md (P8) and track the issue.
      *Not done:* `power_now` before-suspend vs after-resume per cycle. EPP is the cause and is read
      directly; a per-cycle wattage delta would need a settle window inside each of 25 cycles.

**Gate:** 25× suspend/resume mid-playback (`just soak`): alive, position correct, audio back, no dim.
*(Partially met 2026-07-10: `just soak` ran 10 cycles — alive + position correct on every resume,
T5. The full 25× with the audio-back and no-dim clauses is still ungated.)*

**Engine bridge (keystone):** `launcher/src/devtools.{hpp,cpp}` — dependency-free CDP client (raw
sockets + hand-rolled RFC 6455) discovering the page target and running `Runtime.evaluate`. Shared by
the suspend/resume/idle hooks. `devtools_test` covers the frame codec + a loopback CDP server.

## Phase 7 — Widevine path  (4–6 ED, best-effort)   → `launcher/` CDM fetcher (S0.3 gated)

- [~] Re-enable `enable_library_cdms`/`enable_widevine` if stripped; register desktop CDM host.
      **`enable_widevine = true` added to `args/deck.gn`** (S0.3: path intact, only gated off).
      `enable_library_cdms` left out until a build says it's required.
      **Registration gap (2026-07-09 review, static):** the GN arg alone CANNOT make the engine load
      a CDM — registration is `ContentClient::AddContentDecryptionModules` (empty default), and
      neither `content/shell` nor `cobalt/shell` overrides it, so the `CdmRegistry` stays empty and
      `com.widevine.alpha` key-system requests are rejected. **P7 needs a small `patches/` entry**
      (a shell-side `AddContentDecryptionModules` override pointing at
      `CdmFetcher::installed_path`) — the first real patch in the series. See m114.md "Widevine
      registration gap".
      **DONE 2026-07-09: `patches/0001-…-widevine-cdm-registration`. LINKED + RUN 2026-07-10.**
      Applies cleanly to a pristine `DEPS.pin` tree; `gn gen` accepts the new
      `//third_party/widevine/cdm:headers` dep; `shell_content_client.o` compiles under the `deck`
      preset with `BUILDFLAG(ENABLE_WIDEVINE)` live and the symbol in the object file. The launcher
      passes `--widevine-cdm-path` only when a CDM is actually installed.
      Two corrections to the original note, both of which would have produced a patch that compiles
      and does nothing: the `CdmCapability` **must be concrete** at registration (Linux's
      `LazyInitializeCapability` hands back `absl::nullopt`, so a lazily-initialised CdmInfo is
      registered and then rejected, silently), and `enable_library_cdms` is **already true**.
      **BUILT INTO A REAL ENGINE AND RUN 2026-07-10.** Until then only `shell_content_client.o` had
      been compiled — `out/deck/content_shell` still predated the patch, and every result reported on
      2026-07-09/10 (smoke, cert 45/46) came from an *unpatched* binary. The engine was rebuilt and
      both gates re-run against it: `just smoke deck` passes, `just cert conformance-test deck` is
      unchanged at 45/46 with only the documented `MediaElementEvents`. So the patch compiles, links,
      and regresses nothing. Measured over CDP on the patched engine:
      `requestMediaKeySystemAccess('org.w3.clearkey', …)` **resolves**, and
      `requestMediaKeySystemAccess('com.widevine.alpha', …)` **rejects with `NotSupportedError`**
      when no `--widevine-cdm-path` is given — which is precisely what the patch should do with no
      CDM installed. `--widevine-cdm-path` and `--widevine-cdm-version` are both present in the
      binary.
      Not verified: that a real CDM loads, that a licence exchange succeeds, or that any DRM video
      plays. That needs Google's CDM, which CI must never fetch (docs/legal.md), so it stays a
      user-side step. `tests/deck/test_media.py::test_widevine_state` is the test that will say so —
      it is still expected to fail, now for the *right* reason (no CDM installed) rather than the
      structural one (no registry entry possible).
- [~] First-run fetcher downloads Google's Linux x64 CDM (verify hash) → profile dir. Never redistributed.
      **`CdmFetcher` implemented** (`cdm_fetcher.cpp`): detects an installed CDM at Chrome's
      `WidevineCdm/_platform_specific/linux_x64/libwidevinecdm.so`; **opt-in** download from a
      user-configured `cdm_url`, **SHA-256-verified** against `cdm_sha256` before install (self-contained
      `sha256.{hpp,cpp}`, FIPS vectors tested). We never bundle/redistribute Google's CDM. **Verified
      end-to-end offline** via a `file://` URL (detect-only / wrong-hash-rejected / verified-install /
      re-detect). *TODO: the Google component-updater endpoint (a CRX/zip, not a raw .so) — the direct-URL
      path is the MVP; document in SUPPORT.md.*
- [~] Honest UX: L3 caps DRM'd res; free YT unaffected; graceful error if CDM missing/rejected.
      **Honest messaging DONE** (actionable log lines when no CDM/url; free-YT-unaffected stated; hash
      mismatch refuses). L3 resolution-cap UX is Deck/DRM-stream work, still TODO.

**Gate:** a Widevine test stream + one YT rental play; human-readable error when CDM absent.

## Phase 8 — Packaging & distribution  (5–7 ED)   → `flatpak/`, `scripts/`   (starts after P1)

- [x] Flatpak on `org.freedesktop.Platform//25.08`; sandbox via **zypak** (no `--no-sandbox`).
      **Built and installed for real 2026-07-10.** The `pack` image left Debian 12 for `debian:13-slim`
      (flatpak 1.16.6 = the Deck's, flatpak-builder 1.4.4, appstreamcli 1.0.5, dbus); it is the one
      image with no reason to match Cobalt trunk, because flatpak-builder compiles every module inside
      the SDK sandbox. The metainfo now installs and `appstreamcli compose` produces `share/app-info/`.
      Runtime moved 24.08 → 25.08: 22.08 and 23.08 already carry EOL markers on Flathub, two branches
      are supported at a time, and 26.08 lands next month. The 25.08 SDK is GCC 15.2.0 and the C++23
      launcher is `-Werror` clean under it. Design + measurements:
      `.internal/findings/durable/packaging-toolchain.md`.
- [x] Minimal permissions (network, x11+ipc, pulse, dri, `--device=input`, ScreenSaver, login1).
      **`--device=input` is now baked into `finish-args`** (never `--device=all`), together with
      `--share=ipc` (Chromium's X11 MIT-SHM path) and `--require-version=1.16.0` (flatpak learned
      `--device=input` in 1.15.6, so a 1.14 host is refused instead of handed a dead gamepad).
      Verified by reading the sandbox back out of the committed ostree object *and* out of a real
      install: `devices=dri;input;`. `just flatpak` refuses to bundle an app that cannot see
      `/dev/input`, and `scripts/flatpak-lint.sh` fails the build if the manifest ever drops it.
- [ ] Widevine as extra-data / first-run fetch (legal note in `docs/legal.md`).
      *(CdmFetcher opt-in path done — see P7; extra-data manifest wiring is the remaining piece.)*
- [ ] Self-hosted runner for Chromium-scale builds + persistent sccache; free runners = lint only.
- [~] `just install` → flatpak + `steamos-add-to-steam` + controller-layout steps + grid art.
      The `flatpak override --device=input ... && info || info "note: not applied"` is **gone**: both
      branches returned 0, the failing one swallowed flatpak's error, and `just install` reported
      success while handing the user a dead gamepad and a touchscreen lock that silently did nothing.
      It now reads the installed permissions and exits 2 if evdev is absent (F1 in miniature).
      The bundle carries `--runtime-repo=flathub`, so a Deck that has never seen `Platform//25.08`
      pulls it on install — verified against a fresh flatpak user dir.
      `install.sh` does install + evdev verification + `steamos-add-to-steam` + prints the
      controls/layout/DRM steps (refreshed for the new `.vdf` + touch chord). **`docs/SUPPORT.md`
      now complete** (controls table, layout import, touch lock, DRM opt-in, sign-in, logs). Grid art
      still TODO.
- [ ] **Document Steam Input as the user remap path** in SUPPORT.md (input-ux §7): rebinding a
      physical control to a different xinput button in Steam's configurator remaps Deckback without
      touching app.json — users can't realistically edit JSON in Game Mode; `keymap` stays the
      semantic (hotfix) layer, Steam Input is the per-user layer.
- [ ] **SUPPORT.md text-entry & keyboard section** (input-ux §8.3) — safety-relevant, not cosmetic:
      open the on-screen keyboard from the **QAM ("…") menu, never `STEAM+X`** (on a non-Steam
      Chromium app it can raise the desktop OSK and **soft-lock the session**); the keyboard **never
      auto-appears** when a text field is focused; a physical/BT keyboard works but Game Mode forces
      **QWERTY scancodes** regardless of the configured layout.
- [ ] **State the two Deck-Verified requirements we structurally cannot meet** (input-ux §9), in
      SUPPORT.md/README rather than implying Verified parity: (a) **glyph correctness** — Leanback
      renders its own generic/remote glyphs server-side and we cannot map them to Deck glyphs;
      (b) **auto-invoked OSK on text input** — needs Steamworks (we don't link it) or Wayland
      `text-input-v3` (we run Xwayland).
- [ ] **SUPPORT.md battery/performance section** (after the P4 QAM sweep lands numbers): recommend
      OLED refresh 60 Hz for video + framerate cap + optional TDP cap; note the post-resume EPP bug
      on affected SteamOS builds (P6 measurement); troubleshooting entry: `LD_PRELOAD=""` in the
      shortcut's launch options if Game Mode develops hitches an SSH launch doesn't show (Steam
      overlay preload — `durable/platform.md`).

**Gate:** clean SteamOS (**OLED**): GitHub release → playing in Game Mode in < 5 min, one command.
LCD is best-effort/untested and cannot gate a release we have no way to validate.

## Phase T — Test automation, conformance & hardware TDD   → `.internal/TEST-PLAN.md`, `tests/deck/`

The keystone gap: **L0 (unit) and L1 (headless smoke) exist; L2 (on-Deck automated) does not**, so
every "on-Deck: TODO" elsewhere in this file is blocked on it. The two on-Deck scripts we have are
not closed-loop — `soak.sh` only checks `pgrep` liveness (it carries a literal
`TODO(Phase 6): assert playback position advanced`) and `power.sh` asks a human to "play a 1080p VP9
video now". Read `.internal/TEST-PLAN.md` before starting: it holds the tier definitions, the honest
tested/untested matrix, the hardware-TDD loop, and the probe snippets.

- [x] **T1 — `scripts/cdp.py` is now an importable library. DONE 2026-07-09.** Exports `CDP` (context
      manager, `close()`, `wait_for`, `pump`, `screenshot`, `set_user_agent`), `key_spec` (named keys
      **and** any single printable char, carrying the `text` field that makes `beforeinput`/`textInput`
      fire), `dispatch_key(name, modifiers)`, **`dispatch_raw(key, code, vk, …)`** so the §8.4 spikes
      can inject their mandatory negative controls (CEA 461/415), `type_text`, `click`/`mouse`, and
      **`MediaState`** — the `Media`-domain accumulator implementing the T4 probe. **`CDP.call` now
      feeds unsolicited events to `MediaState` instead of dropping them**; that drop is precisely why
      the decoder identity was unobtainable before. The CLI is unchanged for `smoke.sh` but now obeys
      the HARNESS.md exit taxonomy (`2` assertion / `4` transport / `5` usage), where a transport
      failure previously escaped as a traceback and exit 1 — indistinguishable from a real regression.
      No Selenium/Playwright (minimal-deps rule; they can't run in the build image).
      Pure logic covered by `tests/harness/test_cdp_lib.py` (`just test-harness`). Mutation testing
      found a hole: every software-decoder case also set `kIsPlatformVideoDecoder=false`, so deleting
      the `"Vaapi" in name` check left the suite green. A dedicated case now pins it.
- [x] **T2 — `tests/deck/` pytest harness + `just test-deck [args]`. BUILT 2026-07-09; FIRST RAN
      2026-07-10, blocked by Leanback's guest-account gate.**
      23 tests. The 2026-07-10 run reached the OLED unit (after the `DECK_HOST`/`reachable()` fix,
      harness.md F14) but stalled at Leanback's "watch as guest" account wall, so **almost nothing in
      TEST-PLAN §2 moves out of "unverified" until this runs green past the account gate** — a
      `conftest.py` fixture that dismisses the guest gate is the next unlock. Session-scoped
      `ssh -L 9222:localhost:9222` tunnel (Chrome ≥111 requires `Host: localhost` on `/json`), clean
      skip when no Deck, `--no-skip` to make a missing Deck an error for the unattended runner,
      per-test artifacts (screenshot + `journalctl --user` + app log) on failure.
      `scripts/test-deck.sh` maps pytest's exit codes onto the harness taxonomy: failed test → 2,
      no Deck/no pytest → 3, unreachable Deck → 4.
      Markers: `gate` (must pass; a failure is a product defect) vs `probe` (discovery of something
      §2 lists unverified; a failure is a finding). Nothing is `xfail`ed — an xfail hides the answer
      the probe exists to produce.
      *Deferred:* `pytest-rerunfailures` (`only_rerun=["ConnectionError","TimeoutError"]`) — the
      dependency is not installed anywhere yet, and retrying an `AssertionError` would be worse than
      not retrying at all.
- [x] **T3 — Synthetic input lib (`tests/deck/lib/uinput.py`). DONE 2026-07-09.** Raw `struct` +
      `fcntl.ioctl` against `/dev/uinput`, in a program generated here and piped to the Deck's
      `python3` over stdin — nothing installed there, nothing left behind. Pure helpers unit-tested
      at L0 (`tests/harness/test_deck_lib.py`, findings harness §F7), which caught **`struct
      input_event` being packed at 16 bytes instead of 24** before any hardware existed: with
      `struct`'s `=` prefix `l` is 4 bytes, and the only on-hardware symptom would have been "the
      device exists and nothing happens".
      Gotchas honoured: `EV_SYN` is unrepresentable by construction (not a runtime check that could
      never fire); every `EV_ABS` axis needs a full 4-tuple `AbsInfo`, validated; `/dev/uinput`
      failure exits **3 (ENV)** so a missing udev rule is never filed as a product bug; a 2.5 s settle
      matches `input.cpp`'s 2 s hotplug rescan.
      `capabilities_program()` answers three separate registered unknowns in one read: does the Steam
      virtual pad expose `ABS_RX/ABS_RY` (§15), analog `ABS_Z/ABS_RZ` (§12), and `EV_FF/FF_RUMBLE`
      (§14)?
- [x] **T4 — Decoder-identity probe via the CDP `Media` domain. BUILT 2026-07-09; RAN 2026-07-10,
      no verdict yet (account-gate blocked).**
      `scripts/cdp.py:MediaState` accumulates `Media.playerPropertiesChanged`;
      `tests/deck/lib/probes.py:hardware_decode_verdict` adjudicates, once, and
      `tests/deck/test_media.py::test_hardware_decode_is_vaapi` is the P4 gate. Fails on
      `Dav1dVideoDecoder`/`VpxVideoDecoder`/`FFmpegVideoDecoder`, and on a **missing** value —
      "the Media domain never reported" is a broken probe, not working hardware.
      A second copy of the verdict lived in `cdp.py` for a week and had already drifted: it read a
      missing `kIsPlatformVideoDecoder` as "software decode", which sends the reader to debug VA-API
      when the real fault is a probe that has not finished reporting. Deleted; one adjudicator.
      This is also the only honest test of the AV1 dispute — it asserts on the decoder actually used,
      not on the profiles libva advertises. The 2026-07-10 run reached the OLED unit but Leanback's
      guest-account gate kept a real video from playing, so it has not yet returned a decoder verdict;
      that waits on the account-gate fixture (T2).
- [x] **T5 — Close the loop on `soak.sh` and `power.sh`. `soak` RUN + PASSED on-Deck 2026-07-10;
      `power` blocked on Deck state.**
      `just soak` ran for the first time on 2026-07-10 (OLED): 10 suspend/resume cycles
      (`rtcwake -m mem -s 45`), and on **every** resume the app was alive, the video kept playing,
      and `currentTime` had advanced — P6's core clause verified. EPP unchanged across resume on this
      build. Still NOT checked by the gate: audio actually restored, screen did not dim (the two
      remaining P6 clauses). Prereqs discovered running it: passwordless `sudo rtcwake` (a
      `zz-`-prefixed sudoers drop-in, because SteamOS's `deck ALL=(ALL) ALL` otherwise wins the
      last-match), and `reachable()`/`DECK_HOST`-export both had to be fixed first (harness.md F14/F15)
      — which is the real reason it had "never run" before today. `just power` still unrun: the P4
      gate now correctly refuses on AC or with a dark panel (F16), and the Deck was on AC.
      New `scripts/deckctl.py` (workstation-side, opens its own `ssh -L` tunnel) answers three
      questions with an exit code: is a video actually advancing, what is `currentTime`, which
      decoder is the engine using, and what is EPP on every core.
      `just power` now refuses to sample unless a video is playing **and** the decoder is VA-API, and
      re-checks playback after the window. A 300 s average over a *paused* video passes the 9 W gate
      effortlessly and describes nothing — F1 one layer up, a measurement of the absence of the thing
      being measured. A draw measured under `Dav1dVideoDecoder` is a number about software we do not
      ship.
      `just soak` now reads `currentTime` before each suspend and requires it to have advanced after
      the resume. It used to check only that a *process* existed, which a launcher resuming into a
      frozen black frame passes with full marks. Backwards movement is reported as "the player
      restarted", not as a stall: both fail, but only one sends you hunting a hang that never happened.
      EPP is **measured and reported, never gated** — SteamOS resets it on cores 1–7 after resume
      (steamos#2383) and writing it back needs root, so it is infeasible from the Flatpak. A gate you
      cannot pass and cannot fix is a gate someone disables, and then it stops reporting too.
      Paused is ENV (3); stalled is ASSERT (2); a silent Media domain is ENV, not software decode.
      All three verdicts are L0-tested (`tests/harness/test_deckctl.py`) and mutation-checked.
      *Still not checked, and the P6 gate still says so:* audio actually restored, screen did not dim.
- [x] **T6 — Capability probes in `just smoke` (the R1/R2 nightly canary). DONE 2026-07-09.**
      `--assert-ua` (the override survived the navigation; when it silently does not, every later
      probe measures the desktop site) and `--assert-av1-steering` (AV1 unsupported through
      `MediaSource.isTypeSupported`, `canPlayType`, **and** `mediaCapabilities.decodingInfo`, while
      VP9/H.264 stay supported — the second half is a mandatory negative control).
      The steering script moved to `config/av1_steering.js` and is now the single source of truth:
      CMake compiles it into the launcher, and smoke injects the same file, so the canary exercises
      the script that ships. Before injecting, smoke asserts the **unsteered** engine reports AV1 as
      supported: on a Cobalt build with no AV1 decoder every steering assertion would otherwise pass
      while proving nothing — F1 in a different costume.
      Every predicate must evaluate to exactly `true`. `undefined` from a misspelt property is truthy
      under no test and equal-to-true under a sloppy one, and neither reading is the truth.
- [x] **T7 — Self-host the YouTube conformance harness. BUILT 2026-07-09; RUN 2026-07-10.**
      `just cert conformance-test deck` completes 46/46: **45 PASSED, 1 FAILED**, baseline committed.
      The failure (`MediaElementEvents`) is a property of `js_mse_eme` against Blink's MSE, proved by
      instrumentation and by falsification, and documented in finding F11.
      (The 2026-07-09 wording here claimed "there is no `content_shell` on this machine". That was
      false when written — the engine was built the same day — and it reached a commit message.
      Correcting it in place, because a status doc that silently rewrites its own history is worth
      less than one that shows what it got wrong.)
      Re-verified 2026-07-09: the public runners are still dead (`ytlr-cert.appspot.com`,
      `yts.devicecertification.youtube` → sunset stub; `yt-dash-mse-test…/unit-tests/<year>.html` →
      404) and `github.com/youtube/js_mse_eme` is still live (archived read-only 2026-04-19), pinned
      at `c1ef8579` in `tests/cert/HARNESS.pin`. The GCS media bucket answered 200.
      **Passing it is NOT YouTube certification** and must never be described as such (docs/legal.md).
  - [x] **T7a — Hosting + reverse tunnel. DONE.** `cert.py` serves the pinned harness from
        `127.0.0.1` only (never the LAN) and exposes it to the Deck with `ssh -R`. The tunnel is
        load-bearing twice: EME needs a secure context, which `http://localhost` has and a LAN IP
        does not, so a LAN origin fails every EME test for a reason unrelated to our engine; and the
        harness POSTs results to `qual-e.appspot.com` when the page URL contains
        `appspot.com`/`googleapis.com` (`harness/test.js:387`). `PREFLIGHT_EXPR` asserts
        `isSecureContext` and the absence of both markers *before any test runs*.
        Verified end-to-end against the live bucket on 2026-07-09: server, `util.js` rewrite, a real
        6 MB vector, HTTP range requests, cache hit.
  - [x] **T7b — Mirror + hash-pin the media vectors. DONE.** `harness/util.js` is rewritten at serve
        time so `MEDIA_PATH`/`CERT_PATH` point at our own origin, and the rewrite **raises** when
        either constant is absent. That raise is the point: a silent no-op would send all ~244
        vectors straight back to Google's bucket, unhashed, and the run would still go green.
        Vectors are fetched on first miss into `.cache/cert-vectors/`, recorded by
        `--freeze-vectors` into `tests/cert/vectors.sha256`, and verified on every later run. A
        mismatch exits **3** — "Google changed the bucket" and "our decoder broke" are different
        sentences, and at 3 a.m. only one is worth waking up for. We fetch and verify; we never
        redistribute (the `CdmFetcher` posture).
  - [x] **T7c — `scripts/cert.py`. DONE.** Built on the `cdp.py` client, no new deps.
        **It scrapes `globalRunner.testList`, never `getTestResults()`.** The harness's own helper
        buckets tests into `pass`/`fail` and silently drops `UNKNOWN` and `TIMEOUT`, so a suite
        wedged on test 3 of 400 reports zero failures (`harness/test.js:619`). Hang detection is two
        independent deadlines: a per-suite wall clock, and a no-progress watchdog that screenshots,
        names the in-flight test, and aborts. `stoponfailure` stays off — we want the whole result
        vector. Emits JUnit XML + JSON + screenshot.
  - [x] **T7d — Expectations baseline. RECORDED 2026-07-10** from `out/deck/content_shell`:
        `conformance-test` 46/46 completed, **45 PASSED, 1 FAILED**. The one failure,
        `MediaElementEvents`, is a property of `js_mse_eme` against Blink's MSE and not of our build
        (finding F11), and it is in the baseline as `FAILED` with that reasoning attached. The other
        two failures of the first run were *ours*: a `/echo` route the harness needs and upstream
        never ships, and its 10-second timeout knocking over the next test. Only a **regression**
        fails the build: a test that was `PASSED` and is not.
        A newly-passing test prints "baseline update available" and never auto-updates;
        `--update-baseline` is refused when `$CI` is set, because a baseline that rewrites itself
        agrees with whatever just happened, and that is not agreement.
        An `UNKNOWN` **fails the run**, separately from any regression: a suite that stopped after 3
        of 400 tests has no opinion about the other 397, and silence is not assent.
        Failed indices are re-run (`command=run:3,17,42`); a test that then passes is recorded
        **`flaky`**, never `pass` — §0's rule reaching L3, retry transport and never adjudication.
        `harness_commit` and the vector count go into the result JSON: a baseline whose harness you
        cannot name is not a baseline.
  - [x] **T7e — Two tiers. DONE.** `just cert [suite] [preset]` runs headless in the build container
        (Xvfb + `content_shell`, `--autoplay-policy=no-user-gesture-required`), **no Deck** — MSE and
        codec correctness are engine behaviour, and that is what keeps the gate cheap enough to
        survive a busy week. `just cert-deck [suite]` drives a Deck that is already running the app.
        `playbackperf-*` lives in `TREND_SUITES`: recorded, exit 0 regardless. TDP, refresh rate, and
        thermals make it nondeterministic on a handheld, and a nondeterministic gate is a gate
        someone disables. Pair a perf drop with the T4 decoder probe to attribute it — `playbackperf`
        down *and* `kVideoDecoderName` flipped to `Dav1dVideoDecoder` is a steering bug, not heat.
  - [x] **T7f — Widevine stays out of the automated gate. DONE by construction.** No Widevine suite
        appears in `GATE_SUITES` or `TREND_SUITES`, and an L0 test asserts it stays that way. CI must
        never fetch Google's CDM (docs/legal.md), so the unattended run is ClearKey only; Widevine
        key systems remain expected-fail until the P7 `AddContentDecryptionModules` patch lands.

- [x] **T11 — Unattended on-Deck runner + regression subset. BUILT 2026-07-10; NEVER MET A DECK.**
      `just deck-ci` = deploy → launch → `gate` → `cert` → teardown; `--full` adds `probe`, `power`,
      `soak`; `--dry-run` prints the plan and the exit-code policy and touches nothing. The
      **regression subset** is `test-deck.sh -m gate --no-skip` plus `cert --deck conformance-test`.
      Exit codes are **aggregated**, not concatenated: a regression (2) outranks a later SSH drop (4)
      so a retry cannot bury a defect; only TRANSPORT is retried (never a test result, never ENV); a
      failed *blocking* stage halts the run instead of emitting a wall of red with one cause; and a
      run in which **nothing adjudicated exits 3, never 0** — F1 generalised, and the loudest test in
      `tests/harness/test_deckci.py`. `probe` failures are reported and never fail the build, exactly
      as `conftest.py` promises. All judgement is in the pure `scripts/lib/deckci.py`; the half that
      needs hardware makes no decisions (F7's rule, applied to the thing that drives F7's harness).
      Surfaced and fixed on the way: `argparse` exiting 2 (= "the product is wrong") on a typo,
      `lib.sh` not exporting `DECK_HOST` so teardown silently no-opped, teardown bypassing `deck_ssh`,
      and `just smoke` reporting a missing build as TRANSPORT (F12).
- [ ] **T8 — Chromium-native media tests against the `content_shell` we already build**:
      `media_unittests`, and Blink/WPT `media-capabilities/` (`decodingInfo.any.js` exercises exactly
      the surface our AV1 steering overrides) + `encrypted-media/`. Note `cobalt/black_box_tests` is
      **removed on Chrobalt trunk** (Starboard-only, `25.lts`), and `starboard/nplb` still compiles
      but no longer exercises real decode (Chromium media pipeline) — don't plan around either.
- [ ] **T9 — Write the failing test first for every D-list feature** (TEST-PLAN §4): text entry, OSK
      erase, scan keys, voice, gestures, context layers, error page, first-run overlay, latency,
      Widevine. The Widevine one is exemplary: `requestMediaKeySystemAccess('com.widevine.alpha')`
      must fail today (empty `CdmRegistry`) — **that failing test is the spec for the first
      `patches/` entry.**
- [ ] **T10 — Register two milestone findings** (`m114.md`): (a) the exact VA-API decoder string this
      pin emits; (b) whether Steam Input re-wraps a *newly created* uinput pad (double-input risk) —
      currently unverified and it affects every T3 test.

**Gate:** `just test-deck` green on a clean boot, twice; `just cert` runs **unattended** (exit code,
no human) with zero regressions against the committed baseline on `conformance-test` +
`msecodec-test`; the §2 status matrix in TEST-PLAN.md has no rows left in column C.

**Sequencing (cheapest-first, each unblocks the next):** T1 → T2 → **touch-lock grab test** (it
decides whether the touch-lock feature exists at all) → L2/R2 modifier layer → T4 decoder probe →
T7a–T7e cert → gesture layer (only if its §11 price is accepted). Do **not** start the gesture layer
before the grab test passes: it is built entirely on the assumption that grabbing works.

## Phase 9 — Test & hardening  (5–7 ED)

- [ ] Run the **OLED-only** matrix (doc §6 P9): playback, input, voice, power, network, account, DRM,
      update, docked-smoke. **Locked 2026-07-09: there is no LCD unit and none is planned**
      (`findings/durable/decisions.md`), so the LCD column is permanently empty. LCD ships as
      **best-effort/untested**, and code stays model-agnostic — only our claims narrow.
- [ ] **Recruit an LCD owner for the beta.** This is the only realistic path to LCD coverage and is a
      **scheduling dependency**, not a nice-to-have. Their first three jobs: dump the touchscreen
      VID:PID, run `just cert`, and report `kVideoDecoderName` during 1080p playback.
- [ ] **Dump the OLED touchscreen's real VID:PID/name** and fix `touch.cpp` if it differs. The pinned
      `2808:1015` (FTS3528) comes from a **blog post about an LCD Deck**, not from our unit — dropping
      LCD concentrates this risk rather than removing it (`findings/durable/hardware.md`).
- [ ] Small beta group before Flathub; fix window.

## Phase 10 — Flathub submission & maintenance  (3–4 ED + ongoing)

- [ ] Flathub review (sandbox/zypak, extra-data, trademark).
- [ ] Track `youtube/cobalt` trunk monthly; keep UA/keymap remotely overridable & signed for hotfixes.
- [ ] Issue templates, `docs/SUPPORT.md` log-collection script.

---

## Phase 11 — Focus preview  (1 ED spike, then fork)   (findings `durable/preview.md`)

Investigate the TV focus/hover video preview (moving focus onto a tile plays a short muted inline
preview or animated thumbnail). **Unlike PiP (`durable/pip.md`), there is NO engine blocker** —
muted autoplay is enabled, there is no per-page concurrent-`<video>` cap, and a preview is ordinary
in-page content needing no window backend (all tree-verified, m114.md "Focus/hover preview"). The
feature is Leanback's own; the only questions are whether it exists **under our Cobalt UA** and
whether **our own AV1 steering / UA is suppressing it** — both unknowable on a workstation. So this
phase is **probe-first**, in the exact shape of the touch-lock grab test: **one on-Deck spike decides
whether the feature exists at all before any code is written.** No `patches/` entry is implied in any
outcome.

- [ ] **P11.1 — The deciding spike (OLED Deck + live `youtube.com/tv`, cannot be closed on a
      workstation).** Run the `durable/preview.md` §1 enumeration probe (a `Runtime.evaluate` that
      dumps `<video>` elements, focused-tile `<img>` srcs, `videoCount`, `h5vcc`, UA, and focused-tile
      `data-*`) **three times** — pre-focus, on-focus, after ~3 s dwell — and **diff them**. Dismiss
      the account gate first (`ArrowDown`,`Enter`; m114.md). Assert on **Leanback's** state changing
      (a `<video>`/`<img>` that appears or swaps), never on a value we chose (TEST-PLAN §0). Classify
      the mechanism: (a) animated `<img>`, (b)/(c) muted inline/MSE/DASH `<video>`, or (d) nothing.
- [ ] **P11.2 — Suppression A/B (patch-free).** Re-run P11.1 with `config/app.json
      steer_av1_unsupported:false`, and again with a different TV UA (m114.md PS4/Tizen strings via
      `cdp.py --ua`). A preview that appears only with steering off ⇒ **AV1 steering suppresses it**;
      only under another UA ⇒ **our Cobalt UA suppresses it** (voice-search shape, input-ux §13.2).
- [ ] **P11.3 — Focus vs hover.** If P11.1 shows nothing on D-pad focus, dispatch a synthetic
      `Input.dispatchMouseEvent {type:"mouseMoved"}` at the focused tile's `getBoundingClientRect()`
      centre (CDP coords are CSS-viewport px, no letterbox transform) and re-probe. Preview only after
      hover ⇒ **hover-gated, we have no hover by default** — a launcher focus→`mouseMoved` synthesizer
      would be needed (real code, no patch).
- [ ] **P11.4 — Cost (only if P11.1 finds a preview `<video>`).** Chain the T4 `Media`-domain probe
      (TEST-PLAN §5) on the preview `playerId`: assert `kVideoDecoderName` / `kIsPlatformVideoDecoder`.
      `Dav1dVideoDecoder` = AV1 in software (worst case + steering bypassed); `VaapiVideoDecoder` = hw.
      Then `just power` delta (OLED-only; the P4 gate is still not closed-loop, TEST-PLAN §E) — a
      grid-browse preview is usually a **single** decode (no main video), so the concurrent-decode
      unknown may never be exercised.
- [ ] **P11.5 — Register the fork in `durable/preview.md`.** Per the §6 outcome table: *already works*
      (document; watch for our single-`<video>` play-state model breaking on a 2nd element) · *config
      suppresses it* (config decision, patch-free — but steering stays default-on for LCD-safety, so
      likely *accept no preview*) · *hover-gated* (launcher synthesizer) · *nothing* (candidate (d):
      a **page**-layer dead end, not an engine one — register like the voice/PiP dead ends and stop).

**Gate:** P11.1 captured on a clean boot with a definitive (a/b/c/d) classification, and the §6
outcome recorded in `durable/preview.md`. **Do not write any preview code before P11.1 classifies the
mechanism** — like the gesture layer waiting on the grab test, everything here rests on an assumption
only the live app can confirm.

**Sequencing:** independent of P2–P10; needs only a working on-Deck Leanback (have it, S0.4) and the
L2 CDP harness (Phase PT). Cheapest possible spike; run it opportunistically whenever the OLED Deck
is attached.

---

## Dependency graph (doc §7)

```
P0 ─► P1 ─► P2 ─► P3 ─► P9 ─► P10
        ├─► P4 (S0.5 gated) ─┤
        ├─► P5 ──────────────┤
        ├─► P6 ──────────────┤
        └─► P7 (S0.3 gated) ─┘
P8 starts after P1, hardens through P9
PT (test automation) starts after P1 and gates P9 — it unblocks every "on-Deck: TODO"
   in P2–P7, so pulling it earlier pays for itself.
```

## Open questions blocking later phases (doc §16)

- [x] Name → **Deckback** (`io.github.properrr.deckback`)
- [ ] A1: sw decode OK for M1, VA-API before public release? (assumed yes)
- [ ] Build infra: workstation-as-CI-runner vs. rented large runner?
- [ ] Beta channel: GitHub-only vs. later Flathub `beta` branch?
- [ ] Deck access: IP + SSH creds, which unit first (LCD/OLED)?
