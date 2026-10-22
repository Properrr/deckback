# Project Plan: Native YouTube (Leanback) Client for Steam Deck
**Engine:** Chromium-based Cobalt ("Chrobalt", official youtube/cobalt, 27.lts / Chromium M138; pin in `DEPS.pin`)
**Target:** Steam Deck **OLED (Sephiroth)** — verified; **LCD (Van Gogh)** — best-effort, untested (no unit; R11). SteamOS 3.x, **Game Mode only**
**Distribution:** GitHub releases + a GPG-signed Flatpak repo on GitHub Pages → Flathub (after test phase)
**Status:** shipping — current release v0.0.10. Per-phase status: `.internal/TASKS.md`; what is verified: `.internal/TEST-PLAN.md` §2.

---

## 1. Locked decisions & working assumptions

| # | Decision | Value | Notes |
|---|----------|-------|-------|
| D1 | Engine base | Chromium-based Cobalt (27.lts, Chromium M138) | Legacy 25 LTS Starboard path rejected (sunset). No public docs — GitHub Actions workflows in `youtube/cobalt` are the de-facto build reference. Bumps follow `.internal/MIGRATION.md`. |
| D2 | Content scope | Free YouTube + best-effort Widevine (L3) | Widevine CE CDM is partner-licensed → unavailable. Realistic path: Chromium **desktop** CDM (`libwidevinecdm.so`), fetched user-side (Flathub Chromium pattern). L3 caps DRM'd content resolution; free YT content is unaffected. |
| D3 | Runtime | Game Mode (gamescope) only | App runs as XWayland client under gamescope. Desktop Mode may incidentally work but is untested/unsupported in v1. |
| A1 | HW video decode | **Hardware decode ships** — VA-API via `--enable-features=VaapiVideoDecodeLinuxGL --use-angle=gl` in `config/app.json:cobalt_flags`; pixel-clean on the OLED, 5.6 W under VP9 (`findings/milestones/m138.md`) | Gated on the DUAL check — decoder identity **and** a pixel verdict; a decoder-name check alone false-passes. Re-run it after every Cobalt bump and fall back to software decode if it regresses (`findings/durable/hardware.md`). |
| A2 | Branding | **Deckback** (app id `io.github.properrr.deckback`) | Cannot use "YouTube" in the app name or ship YouTube logos. |
| A3 | Ad blocking / UI mods | **None in v1** | Keeps Flathub acceptance and takedown risk low. Revisit later as opt-in if desired. |
| A4 | Build host | Your workstation (or CI), **not** the Deck | Measured footprint ≈45 GB (source + one preset + caches), 32 GB+ RAM recommended. Deck is deploy/test target over SSH only. |

---

## 2. Goals / non-goals

**Goals**
- Console-grade YouTube TV (Leanback) experience in Game Mode, on par with PS5's app: full controller navigation, in-UI on-screen keyboard, voice search via built-in mic, seek/skip on shoulder buttons, correct back-button semantics.
- Robust sleep/resume: playback pauses on suspend, network and player recover cleanly on wake.
- Idle/dim inhibition while video plays.
- Hardware decode (VP9/H.264/HEVC via VCN) for battery life; AV1 steered away. Whether the Deck decodes AV1 in hardware is **disputed** — libva on the OLED advertises `AV1Profile0` under `VAEntrypointVLD` (`findings/durable/hardware.md`). Steering stays on because it is free and the LCD is unmeasurable, not because the hardware is known to lack AV1.
- Clean packaging: Flatpak, one-command "add to Steam" with a shipped controller config.
- Sign-in with full account features (subscriptions, history, YT Premium if user has it).

**Non-goals (v1)**
- Desktop Mode polish, window management.
- HDR and >60 fps UI (stretch goals, §10).
- 4K docked output (stretch goal).
- Ad blocking, SponsorBlock, DeArrow-style mods.
- ARM or non-Deck hardware.

---

## 3. Architecture overview

```
┌─────────────────────────────────────────────────────────────┐
│ SteamOS 3.x (Arch-based, immutable /) — Game Mode session   │
│ ┌─────────────────────────────────────────────────────────┐ │
│ │ gamescope (Wayland compositor)                          │ │
│ │  ┌───────────────────────────────────────────────────┐  │ │
│ │  │ Flatpak sandbox (org.freedesktop.Platform 25.08)  │  │ │
│ │  │  ┌─────────────────────────────────────────────┐  │  │ │
│ │  │  │ launcher (C++23 shim)                       │  │  │ │
│ │  │  │  · env/flags/config, UA, page scripts       │  │  │ │
│ │  │  │  · evdev gamepad → trusted CDP keys         │  │  │ │
│ │  │  │  · logind sleep-watcher (D-Bus)             │  │  │ │
│ │  │  │  · idle-inhibit, watchdog, self-update      │  │  │ │
│ │  │  │  ┌───────────────────────────────────────┐  │  │  │ │
│ │  │  │  │ Cobalt (Chromium M138 content_shell)  │  │  │  │ │
│ │  │  │  │  · loads https://www.youtube.com/tv   │  │  │  │ │
│ │  │  │  │  · Blink/V8, net stack (HTTP/3)       │  │  │  │ │
│ │  │  │  │  · DevTools port ← launcher (CDP)     │  │  │  │ │
│ │  │  │  │  · media: VA-API hw decode (ANGLE GL) │  │  │  │ │
│ │  │  │  │  · audio: PipeWire (pulse compat)     │  │  │  │ │
│ │  │  │  │  · mic: getUserMedia → PipeWire       │  │  │  │ │
│ │  │  │  │  · CDM host → libwidevinecdm.so (L3)  │  │  │  │ │
│ │  │  │  └───────────────────────────────────────┘  │  │  │ │
│ │  │  └─────────────────────────────────────────────┘  │  │ │
│ │  └── X11 (XWayland, --ozone-platform=x11) ────────────┘  │ │
│ └─────────────────────────────────────────────────────────┘ │
│  Steam Input (non-Steam shortcut) — shipped config template │
│  systemd-logind (suspend signals) · PipeWire · Mesa/RADV    │
└─────────────────────────────────────────────────────────────┘
```

**Key architectural bets**
1. **Identity:** ship a Cobalt TV-client UA (`config/app.json:user_agent`, currently `Mozilla/5.0 (ChromiumStylePlatform) Cobalt/26.lts.0,gzip(gfe)`), config-driven and hot-swappable, so Leanback serves the TV app and enables its console feature set. This is the single biggest external dependency/risk (§8, R1).
2. **Input at the embedder layer, not page JS:** the launcher translates evdev gamepad state into CDP `Input.dispatchKeyEvent`s, which arrive `isTrusted=true` — no engine patch, works before page JS loads, survives page navigation. (VacuumTube does input with injected JS in Electron.)
3. **Two-level input:** in-app native handling is primary; a shipped Steam Input template ("Gamepad with deck-specific extras") is the transport. Trackpad-as-pointer passes through unmodified; **the touchscreen is inert by default** (`disable_touch`, `findings/durable/touch-lock.md`), with opt-in touch gestures (`touch_gestures`, `findings/durable/touch-gestures.md`).

---

## 4. Hardware/platform matrix

> **⚠ There is no LCD unit and none is planned** (`findings/durable/decisions.md`).
> Every "test both" below is unexecutable. **LCD is best-effort and untested for v1**, not supported.
> Code stays model-agnostic; only our *claims* narrow. See `findings/durable/hardware.md` for which
> facts transfer and which do not.

| Aspect | LCD (Van Gogh) — **untested** | OLED (Sephiroth) — our only unit | Plan impact |
|---|---|---|---|
| APU | Zen2 4c/8t + RDNA2 8CU, 7nm | Same arch, 6nm | Identical code path |
| Video decode (VCN) | H.264, HEVC, VP9 ✔ / AV1 **⚠ disputed, unmeasurable** | Same, but AV1 **measured present** (libva `AV1Profile0`/VLD) | Steer YT to VP9/H.264 (`steer_av1_unsupported`, launcher-side). **Stays default-on permanently** — an OLED-only result cannot license AV1 for untestable LCDs |
| Display | 1280×800 **60 Hz** | 1280×800 90 Hz, HDR | Render 1280×720 letterboxed; cap 60 fps. The 90→60→45 Hz QAM sweep is **OLED-only** — meaningless on a 60 Hz panel |
| Wi-Fi | RTL8822CE (Wi-Fi 5) | QCNFA765 (Wi-Fi 6E) | Resume/reconnect timing differs. Only the OLED chip can be tuned → keep the resume backoff conservative, not fitted to one chip |
| Mic | Built-in dual mic array | Built-in dual mic array | P5 verifies OLED only |
| Touchscreen | untested | touch delivery measured (gamescope touch mode 4; multitouch delivered) | Touch is handled in the page and through gamescope's touch mode, not by device id; LCD behaviour unknown |
| Power | 7 nm — draws more | 6 nm — 5.6 W measured under HW-decode VP9 (≤9 W target) | **Never restate OLED battery numbers as LCD numbers** |
| RAM | 16 GB shared | 16 GB shared | Chromium footprint fine |

---

## 5. Development workflow (build host + Deck over SSH)

*(Toolchain details: §13 · container environment: §14 · scripts: §15 and `.internal/HARNESS.md`.)*

**Build host (your machine):**
- `depot_tools`, `gclient` checkout of `youtube/cobalt` at `DEPS.pin`. The repo's GitHub Actions Linux workflow is the build reference (no public docs exist for Chrobalt — the CI YAML *is* the documentation).
- `just gen` layers `args/common.gn` + `args/<preset>.gn` on Cobalt's `gn.py` platform config (§13.3); `cc_wrapper="sccache"`.
- Expect: ~45 GB disk (measured — source + one preset + caches), a from-scratch build of ~1h40m on 32 threads, incrementals fine with sccache.

**Deck (deploy/test target over SSH):**
- Prereqs on Deck: Developer Mode on, `sshd` enabled, password set for `deck` user. **No** read-only filesystem changes needed — everything lives in `~/` or Flatpak.
- Iteration loop (paths and trade-offs: HARNESS §4):
  1. `just deploy` — rsync stripped build → `~/cobalt-yt/` on Deck; `just run` launches it in Desktop Mode (fast sanity).
  2. `just flatpak` + `just deck-install-dev` (or `just install` on the Deck) for the Game Mode build users get.
  3. Remote debugging: `--remote-debugging-port=9222`, `just debug` (`ssh -L 9222:localhost:9222`), attach Chrome DevTools from the workstation (`chrome://inspect`).
  4. Logs: journald + app log dir; `just logs` tails over SSH.
- Add-to-Steam automation: `steamos-add-to-steam` (present on SteamOS) or direct `shortcuts.vdf` manipulation; sets launch options, compat off, and points artwork.

---

## 6. Phased implementation plan

Estimates are focused engineering days (ED) for one senior engineer. Total ≈ **45–60 ED** to Flathub submission. Phase status lives in `.internal/TASKS.md`.

### Phase 0 — Research spikes & de-risking (4–6 ED)
Goal: kill the unknowns before committing to the architecture.

| Spike | Question | Exit criteria |
|---|---|---|
| S0.1 | Does trunk Chrobalt build + run on generic Linux today? Which Chromium milestone is it on *now*? | `cobalt` binary renders youtube.com/tv on the workstation |
| S0.2 | UA/identity: which UA string yields full Leanback (incl. voice search button, sign-in, settings)? Any device-registration/attestation beyond UA? | Documented working UA + feature checklist |
| S0.3 | Is Chromium's library-CDM path (`enable_library_cdms`, `enable_widevine`) intact in Chrobalt, or stripped for the Starboard `SbDrm` path? | Yes/no + effort estimate for re-enabling |
| S0.4 | Ozone: does Chrobalt trunk support `--ozone-platform=x11`? Wayland? What does it run as under gamescope? | Renders fullscreen under nested gamescope on workstation |
| S0.5 | Media stack: does trunk use Chromium's media pipeline (FFmpeg/mojo) or Starboard `SbPlayer` on Linux? Determines the entire Phase 4 approach. | Documented decode path + VA-API feasibility note |
| S0.6 | Gamepad: does Blink's Gamepad API see evdev pads in Chrobalt? Does Leanback react to it with a console UA? | Yes/no → picks P3 mechanism |

Deliverable: spike findings in `.internal/findings/` (milestone-scoped answers in `milestones/<pin>.md`, re-run on every bump); go/no-go on plan adjustments.

### Phase 1 — Build & bring-up on Deck (5–7 ED)
- Reproducible build: pin a Cobalt commit (`DEPS.pin`); GN arg presets in `args/`, sccache, `just deploy`.
- First run on Deck in Desktop Mode (X11), then Game Mode as non-Steam shortcut.
- Resolution/aspect: force 1280×720 render surface, gamescope letterboxes to 800p. Verify text sharpness (Leanback is authored for 1080p — check scaling quality, consider 1920×1080 render + gamescope downscale if GPU headroom allows).
- Persistent profile dir (`~/.var/app/...` shaped even pre-Flatpak) so sign-in survives updates.
- Crash handling: crashpad on, local minidumps only (no telemetry).
- **Acceptance:** cold boot → Game Mode → launch → browse → play 1080p video with audio, stable 30 min.

### Phase 2 — Session & lifecycle plumbing (3–4 ED)
- Launcher shim owning: env, flags, config file (UA, URL, quality caps), log rotation, watchdog restart-on-crash (Leanback expects app restart semantics like a TV).
- Single-instance lock; deep-link support (`app://<id>` → `youtube.com/tv#/watch?v=<id>`).
- Clean exit path: a controller action + Leanback's own Exit menu item → proper shutdown (Steam overlay "Close game" must also not corrupt the profile).
- **Acceptance:** 50 launch/kill cycles via Steam UI with zero profile corruption.

### Phase 3 — Input: PS5-parity controls (6–8 ED)
Mechanism (from S0.6): the launcher reads evdev and dispatches trusted CDP key events; Leanback consumes them as keys. Actions with no Leanback key (captions, chapter seek, gestures, the OSD) are launcher actions that run page scripts over CDP.

**Mapping (shipped `config/app.json` keymap + `config/steam_input.vdf`):**

| Deck control | Action | Transport |
|---|---|---|
| D-pad / Left stick | Navigate (arrow keys) | key events, auto-repeat with accel |
| Right stick | Fast list traversal (`fast_scroll`) | arrow keys at an analog rate |
| A | Select / OK (Enter) | key |
| B | Back | key |
| X | Play/Pause | key (`MediaPlayPause`) |
| Y | Toggle touch gestures (`toggle_gestures`), with toast + rumble. Ships bound but inert while `touch_gestures` is off (the default) | launcher action |
| LB / RB | Scrub back / forward (Left/Right in player) — **not** a fixed jump; Leanback publishes no fixed-interval seek key (findings input-ux §12) | Left/Right in player |
| LT / RT | Previous / next chapter (`chapter_back`/`chapter_fwd`); a fixed `skip_seconds` skip when a video has no chapters | launcher action (`chapter_seek.js`) |
| ☰ Start | Open the OSD Settings menu (Keys / Updates / About; present whenever CDP is up); holding Y inside it exits the app | launcher action (`osd.js`) |
| ⧉ Select | Toggle captions | launcher action (`toggle_captions.js` drives the player's caption module) |
| Right trackpad | Pointer + click (optional, off by default) | mouse |
| Left trackpad | Scroll lists | wheel |
| Touchscreen | **Inert by default** (`disable_touch`); opt-in gestures (`touch_gestures`) | `no_pointer.js` + gamescope hover / gesture router + gamescope passthrough |
| L4/L5/R4/R5 | Duplicate face buttons for reachability (R4 = Y) | Steam Input template |
| Steam / QAM | Reserved by Steam | — |

- On-screen keyboard: use **Leanback's own OSK** (search, sign-in) — d-pad navigable like PS5.
  Steam's OSK is *not* a safe fallback (findings input-ux §8.3): (a) `STEAM+X` on a non-Steam
  Chromium shortcut can raise the **desktop** OSK and **soft-lock** the session — the reliable path is
  **QAM ("…") → keyboard**. (b) Under `--ozone-platform=x11` the OSK **never auto-appears on `<input>`
  focus** (that needs Wayland `text-input-v3`), and we cannot call the Steamworks OSK API. So Valve's
  "auto-invoke an OSK for text input" recommendation is **structurally unmeetable** — document it,
  don't fake it. Physical/BT keyboards do reach the app, but Game Mode forces **QWERTY scancodes**
  regardless of layout.
- Keycodes: Chrobalt is Chromium → `keyCode` is the **Windows VK** set (`SbKey` == VK). The
  CEA-2014/HbbTV codes (461 GoBack, 415 Play, …) are a **different keycode world** (Tizen/webOS) and
  are not native here; desktop watch-page shortcuts (`j`/`l`/`k`/`/`) are a **third** surface. Google
  publishes no key contract for the TV app — bind nothing without an on-Deck spike (findings §8.1/§8.4).
- Voice search is **not a key**: `kSbKeyMicrophone` does nothing, because Cobalt routes voice through
  a Starboard service this build lacks (findings input-ux §8.2/§13). The design is hold-to-talk → a
  trusted `Input.dispatchMouseEvent` on Leanback's soft-mic button, ducking playback while capturing.
- Ship a Steam Input template (`config/steam_input.vdf`): plain "Gamepad" layout so in-app handling sees raw pad; document how users apply it (non-Steam apps can't push official layouts — provide import instructions + community layout name).
- Rumble: evdev `EVIOCSFF`/`FF_RUMBLE` (`haptic.cpp`, chosen over SDL2 to keep deps minimal). Whether the Steam Input virtual pad implements FF is unverified (TEST-PLAN §2).
- **Acceptance:** every Leanback surface (home, search, player, settings, sign-in) fully drivable with controller only; input latency subjectively ≤ PS5 app; OSK usable end-to-end.

### Phase 4 — Media pipeline & hardware decode (8–12 ED)
> **Status:** format steering and VA-API hardware decode ship on M138; the ≤9 W power target is met
> (5.6 W, OLED). The 4-hour battery acceptance has not been run. See `findings/milestones/m138.md`.
- **Step 1 (sw decode hardening):** confirm VP9 1080p60 software decode headroom; set default max quality 1080p (800p panel) via player settings persistence; measure battery draw baseline. Software decode is the fallback if hardware decode regresses.
- **Step 2 (format steering):** shape `MediaCapabilities`/`canPlayType` responses: report AV1 unsupported; report VP9/H.264 supported so YouTube's ABR picks hw-friendly streams. Patch-free: the launcher injects `config/scripts/av1_steering.js` over CDP (`navigator.cpp`).
- **Step 3 (VA-API):** `--enable-features=VaapiVideoDecodeLinuxGL --use-angle=gl` (both required) selects `VaapiVideoDecoder` against Mesa radeonsi. On M138, ANGLE imports the tiled NV12 VA surface cleanly; on m114 the same flags painted green-band corruption while every automated metric passed. Gate: decoder identity **and** a **pixel** check (`tests/deck/lib/probes.py:video_corruption_verdict`).
- **Step 4 (power validation):** target: 1080p VP9 playback ≤ ~8–9 W total system draw (vs ~12–15 W sw decode). `just power` measures battery voltage × current (the OLED has no `power_now` node).
- **Acceptance:** 4-hour continuous playback on battery from full charge (OLED), no thermal throttling artifacts, no A/V desync after seek storms.

### Phase 5 — Audio & microphone (3–5 ED)
- Audio out: PipeWire (pulse compat layer). Verify: speaker, 3.5 mm, Bluetooth (SBC/AAC), and seamless device switching mid-playback.
- Mic in: `getUserMedia` → PipeWire source. Auto-grant mic permission for `youtube.com` origin only (`mic_autogrant`: CDP `Browser.grantPermissions` — no permission prompts exist in a kiosk shell).
- Voice search e2e: hold-to-talk → Leanback mic UI → speech recognized (YouTube's server-side ASR) → results. OLED mic array + BT headset mics (no LCD unit). **Not a key press** — see §P3 and findings input-ux §8.2. **Withdrawn:** the launcher's hold-to-talk was removed because the mic never worked on hardware; it returns only when reimplemented against a real device.
- Suspend interaction: PipeWire nodes disappear on suspend — ensure graceful stream rebuild on resume (Phase 6 hook).
- **Acceptance:** voice search success in a normal room on the OLED; audio device hot-swap without app restart.

### Phase 6 — Power: sleep, resume, idle inhibit (4–6 ED)
- **Suspend:** subscribe to `org.freedesktop.login1.Manager.PrepareForSleep` on system D-Bus; take a *delay* inhibitor lock → on `true`: pause player (synthesize Pause), checkpoint position, flush profile → release lock → system sleeps.
- **Resume:** on `PrepareForSleep(false)`: wait for network-online (reachability probe with backoff — expect different latencies on RTL8822CE vs QCNFA765), rebuild PipeWire streams, nudge player (Leanback recovers position; verify token refresh for long sleeps > session TTL).
- **Idle/dim inhibit while playing:** hold the logind idle inhibitor while player state == playing; release on pause. Game Mode's dim/suspend timer ignores every inhibitor reachable from the sandbox (R4), so a host-side uinput helper keeps the screen on during playback (`just idle-nudge`, `findings/durable/keep-awake.md`).
- **Acceptance:** 25× scripted suspend/resume loop mid-playback: app alive, position correct, audio back, no zombie network state; screen never dims during a 30-min video; screen dims normally when paused.

### Phase 7 — Widevine path (4–6 ED, best-effort by design)
- The library-CDM path is intact (`enable_widevine=true`); CDM host registration for the desktop CDM is `patches/0001-…` (content_shell has no `AddContentDecryptionModules` override).
- Opt-in fetcher in the launcher: the user sets `cdm_url` + `cdm_sha256`; the launcher verifies the hash and installs into the profile dir. **Never redistributed by us**; CI never fetches it (`docs/legal.md`).
- Expectations set honestly: L3 software CDM → DRM'd content (rentals, some live/originals) plays but is resolution-capped by YouTube policy (typically ≤ 480/720p); free content unaffected. If YouTube requires VMP for rentals, feature is documented as unsupported — free-YT experience is the product.
- **Acceptance:** a known Widevine test stream + one YT movie rental plays; graceful, human-readable error if CDM missing/rejected.

### Phase 8 — Packaging & distribution (5–7 ED)
- **Flatpak:** `org.freedesktop.Platform//25.08`; Chromium sandbox inside Flatpak via **zypak** (Flathub-standard; no setuid, no nested userns). `--no-sandbox` is *not* acceptable for Flathub — zypak is the plan of record.
- Permissions (minimal; the manifest is authoritative): `--share=network`, `--share=ipc`, `--socket=x11`, `--socket=pulseaudio` (PipeWire), `--device=dri`, `--device=input` (evdev gamepads; avoid `--device=all`), `--talk-name=org.freedesktop.ScreenSaver`, `--system-talk-name=org.freedesktop.login1`, `--talk-name=org.freedesktop.portal.Flatpak` + `org.freedesktop.impl.portal.PermissionStore` (self-update), `--filesystem=xdg-run/pipewire-0:ro`.
- Widevine: never shipped; opt-in user fetch (P7, `docs/legal.md`).
- **GitHub:** repo layout (§9). A release carries the `.flatpak` bundle; publishing it triggers `release.yml`, which imports the bundle into the GPG-signed OSTree repo on GitHub Pages. Chromium-scale builds do **not** fit free GitHub runners (disk/RAM): engine and release builds run on the workstation with a persistent sccache cache; hosted runners run lint (`just preflight`) and the Pages publish.
- **Add-to-Steam UX:** `just install` installs the Flatpak, verifies the evdev grant, runs `steamos-add-to-steam`, and prints controller-layout import steps; the one-line installer (`web-install.sh`) also writes tile artwork (original, non-infringing art).
- **Updates:** the self-hosted, GPG-signed OSTree repo on GitHub Pages plus in-app self-update through the Flatpak portal is the channel of record. It must be able to **recall** a bad release, ship **delta** updates, tell users promptly, and keep the runtime current in Game Mode — **without ever adding a sandbox permission** (a permission add strands installed users). Plan and owner decisions: TASKS Phase 8 (U1–U11), `findings/durable/self-update.md`.
- **Acceptance:** clean SteamOS **OLED** unit: install from GitHub release → playing a video in Game Mode in < 5 minutes, no terminal knowledge beyond one command. (LCD: best-effort, untested — R11.)

### Phase 9 — Test & hardening pass (5–7 ED)
Matrix (**OLED only** — no LCD unit exists, R11 / decisions.md):

| Area | Tests |
|---|---|
| Playback | VP9/H.264 hw + sw, 60 fps content, live streams, seek storm, 4 h soak, BT audio |
| Input | Full-surface controller traversal, OSK, touch, external BT pad, layout re-import |
| Voice | Quiet/noisy room, BT mic, mid-playback search (once voice returns) |
| Power | 25× suspend loop, overnight sleep, idle-inhibit, low-battery behavior |
| Network | Wi-Fi drop/roam mid-stream, captive-ish latency, resume-after-sleep reconnect |
| Account | Sign-in (on-device + code pairing), sign-out, multi-account, token expiry after 3-day sleep |
| DRM | Widevine clip, rental (expected caps documented) |
| Update | Flatpak upgrade in place: profile, sign-in, settings survive |
| Docked (smoke only) | HDMI 1080p output sanity — not a v1 feature, must not crash |

- Fix window + a small beta group (r/SteamDeck etc.) before Flathub.

### Phase 10 — Flathub submission & maintenance (3–4 ED + ongoing)
- Flathub review: precedent exists (VacuumTube is on Flathub), but our build is a full Chromium derivative — expect scrutiny on sandbox (zypak), extra-data (the prebuilt engine tarball, `just flathub-prep`), and trademark (name/icon per §12).
- Maintenance posture: track `youtube/cobalt` LTS branches and bump per `.internal/MIGRATION.md`. Leanback is server-side and *will* change under us — UA, key-map and page scripts live in hot-swappable config (`config/app.json`, `config/scripts/`), so breakage is hotfixed with a Flatpak update and no engine rebuild. A remote, signed config fetch is not built.
- Issue templates, `SUPPORT.md` with log-collection script.

---

## 7. Workstream dependency graph

```
P0 spikes ──► P1 bring-up ──► P2 lifecycle ──► P3 input ──► P9 QA ──► P10 Flathub
                   │                            ▲
                   ├──► P4 media (S0.5 gated) ──┤
                   ├──► P5 audio/mic ───────────┤
                   ├──► P6 power ───────────────┤
                   └──► P7 widevine (S0.3 gated)┘
P8 packaging starts after P1, hardens through P9
```
P4–P7 parallelize well if we ever add a second contributor.

---

## 8. Risk register

| ID | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| R1 | Google tightens youtube.com/tv gating beyond UA (attestation/device registry) | Med | Fatal to product | Config-driven identity, hot-updatable; track cobalt-dev + VacuumTube community; worst-case pivot to certified-device UA parity research. Accept openly in README that the app depends on Google's tolerance (same position as VacuumTube/Kodi plugins). |
| R2 | Chrobalt instability / no docs / breaking churn | High | Delays | Pin commits (`DEPS.pin`); treat CI YAML as docs; keep our patches a thin, rebaseable series in `patches/`; bump on LTS cuts per `.internal/MIGRATION.md`. |
| R3 | VA-API regresses on a Cobalt bump (the same flags painted corrupt frames on m114) | Med | Battery goal slips | DUAL gate (decoder identity + pixel verdict) on every bump; software decode is the fallback; format steering works independently of hw decode. |
| R4 | Game Mode ignores idle inhibitors for non-Steam apps | Confirmed | Annoying UX | Nothing reachable from the sandbox inhibits it; a host-side uinput helper (`just idle-nudge`) keeps the screen on during playback (`findings/durable/keep-awake.md`). |
| R5 | Widevine desktop CDM rejected by YT for rentals (VMP) | Med | Feature degraded | Scope framed as best-effort; free YT is the product. |
| R6 | Trademark complaint (name/branding) | Low-Med | Rename/relist | §12 rules: no "YouTube" in name, no logo, "unofficial" in description. |
| R7 | CI cost/scale for Chromium builds | High | Slow releases | Workstation builds with sccache; hosted runners only for lint and the Pages publish; hotfixes via config (no engine rebuild). |
| R8 | Flathub rejects zypak'd custom Chromium | Low | GitHub-only distribution | The GitHub releases + signed Flatpak repo remain primary until resolved; engage reviewers early with manifest draft. |
| R9 | **Text entry is a dead end**: Leanback's grid OSK may not accept printable keys, the Steam OSK never auto-appears under Xwayland, and `STEAM+X` can soft-lock a non-Steam Chromium app | Med | Search/sign-in painful or unusable | Spike printable typing first (findings input-ux §8.4 — it decides everything downstream); document QAM-not-STEAM+X; Steam Input keyboard binds as fallback; native-Wayland/`text-input-v3` as the stretch fix (§10.7). |
| R10 | **Much of the build is unverified on hardware** — L2 runs on the OLED unit but has no known-state fixture, and TEST-PLAN §2.B/§2.C still list host-tested-only and unverified features | High | False confidence | `.internal/TEST-PLAN.md` §2 keeps an honest tested/untested matrix; new features get a failing on-device test first (TEST-PLAN §0). |
| R11 | **No LCD unit exists** — the LCD can never be verified in-house | Certain | LCD users hit untested paths; AV1 dispute permanently open | Ship LCD as **best-effort/untested**, stated in README + SUPPORT.md; keep `steer_av1_unsupported` default-on forever; keep resume backoff conservative rather than fitted to the one Wi-Fi chip we can measure; never publish OLED power numbers as LCD numbers; **recruit an LCD owner for the P9 beta** — the only realistic coverage path, and a scheduling dependency. |

---

## 9. Repository layout

```
deckback/
├── DEPS.pin                 # the Cobalt commit
├── cobalt/                  # gclient-managed checkout (gitignored), pinned via DEPS.pin
├── patches/                 # patch series against the pinned commit (applied with git am)
│   ├── 0001-…-widevine-cdm-registration.patch
│   └── 0002-…-disable-web-tests.patch
├── args/                    # GN arg overrides: common.gn + dev/deck/asan presets
├── launcher/                # C++23 shim: config, input, CDP bridge, sleep-watcher, idle-inhibit,
│                            #   CDM fetcher, OSD menu, self-update, watchdog
├── config/
│   ├── app.json             # UA, URL, quality caps, keymap, engine flags (hot-swappable)
│   ├── scripts/             # page scripts injected over CDP (av1_steering.js, no_pointer.js, osd.js, …)
│   └── steam_input.vdf      # controller template
├── flatpak/<app.id>.yml
├── docker/                  # build/test/dev/pack image + the SteamOS test sim
├── scripts/                 # one script per `just` recipe (.internal/HARNESS.md)
├── tests/                   # harness (L0), deck (L2), cert (L3), js
├── docs/                    # public deliverables: HOW-IT-WORKS.md, SUPPORT.md, legal.md
├── .internal/               # design doc, TASKS, TEST-PLAN, HARNESS, MIGRATION, findings/
└── .github/workflows/       # lint + release-pages (hosted runners)
```

---

## 10. Stretch goals (post-1.0 backlog)
1. **HDR on OLED** — gamescope HDR + Chromium HDR video on Linux is bleeding-edge; revisit on a later Cobalt bump.
2. **Docked 4K/1080p** — render-resolution switch on external display detect; needs hw decode headroom validation for 4K VP9.
3. **90 Hz UI on OLED** (playback stays at content rate).
4. **MPRIS** — QAM/desktop media controls.
5. **Desktop Mode** first-class support.
6. **Sponsor/ad-skip integrations** — deliberately deferred; separate risk decision.
7. **Native Wayland client (ozone/wayland) for `text-input-v3`** — the only clean way to get the
   Deck's on-screen keyboard to auto-appear on text-field focus (findings input-ux §8.3). Collides
   with the S0.4 result (`--ozone-platform=x11` is what works under gamescope today), so it is a
   real engine/ozone investigation, not a flag flip.
8. **Pinch/zoom-to-fill** for the 16:10 panel (letterboxed 720p), mirroring mobile YT.

---

## 11. Milestones

Releases ship today (current: v0.0.10) through GitHub releases and the signed Flatpak repo. Remaining
milestones: the OLED QA matrix and a beta that includes a recruited LCD owner (P9), then Flathub
submission (P10). Per-phase status is in `.internal/TASKS.md`; unexecuted phase gates are listed in
`.internal/TEST-PLAN.md` §2.E.

---

## 12. Naming & trademark guardrails
Rules: no "YouTube"/"YT" in the app id or store name; no red play-button or logo derivatives; description states "unofficial client for YouTube's TV interface, built on the open-source Cobalt engine"; app id like `io.github.<you>.<name>`.

Name: **Deckback** (Deck + Leanback), app id `io.github.properrr.deckback` (A2).

---

## 13. Toolchain: compilers & C++ standards

### 13.1 In-tree (Cobalt/Chromium patches) — the toolchain chooses you
- **Compiler:** Chromium's **hermetic, bundled Clang** — non-negotiable. `gclient sync` hooks fetch the pin's toolchain into `third_party/llvm-build/Release+Asserts/`. GCC is **not supported** by the Chromium build system (distro maintainers carry large patch sets to force it — we won't). Do not substitute a system Clang either: version skew breaks `-Werror`, plugins, and PGO/LTO assumptions.
- **Linker:** bundled **lld** (default). **Stdlib:** Chromium's in-tree **libc++** (`use_custom_libcxx=true`, default) — all in-tree patches compile against it, not the system stdlib.
- **C++ standard: C++17** for anything under `patches/`. Newer-standard features in patches maximize rebase pain on every bump. Chromium style guide applies; format with the tree's own `buildtools/linux64/clang-format` (see `just fmt`).
- **Sanitizers:** `is_asan=true` builds for workstation-side repro of memory bugs; not deployable to the Deck (perf/size). TSan rarely needed for our patch surface.

### 13.2 Out-of-tree (launcher, CDM fetcher, tools) — our choice
- **Standard: C++23.** Small, modern, self-contained codebase.
- **Compilers: dual-clean.** Clang (≥18) and **GCC 14**, because the Flatpak build uses the `org.freedesktop.Sdk//25.08` toolchain (GCC-based). CI (`just preflight launcher`) builds both with `-Wall -Wextra -Werror`; clang-format is pinned (`CLANG_FORMAT_VERSION` in `scripts/lib.sh`). An ASan+UBSan build is one option away (`DECKBACK_ASAN`).
- **Build:** CMake ≥ 3.28 + Ninja. Deliberately *not* folded into the GN tree — the launcher rebuilds in seconds and must never require a Chromium checkout to hack on.
- **Deps (minimal by design):** `libsystemd` (sd-bus for logind/ScreenSaver/portals — systemd is guaranteed on SteamOS), `libcurl` (CDM fetcher, HTTP), `libpulse` (audio), `xcb` (gamescope touch mode), stdlib otherwise; evdev is read with raw ioctls. No Boost/Folly-class deps.

### 13.3 GN args presets (checked into `args/`)

`just gen <preset>` runs Cobalt's `cobalt/build/gn.py -p chromium_linux-x64x11 -c <devel|qa>` and
appends `args/common.gn` + `args/<preset>.gn` (`findings/milestones/m138.md` §"Build system"). Our
files hold **overrides only** — the `gn` preflight target checks that.

- **`args/common.gn`** (every preset): `proprietary_codecs = true` + `ffmpeg_branding = "Chrome"` —
  **required**; without them YouTube's H.264/AAC fallback formats fail with
  `DEMUXER_ERROR_NO_SUPPORTED_STREAMS`. Also `cc_wrapper = "sccache"` +
  `enable_precompiled_headers = false` (PCH defeats sccache), and the feature-off slim set
  (`findings/durable/build-slimming.md` — read it before flipping anything).
- **`args/dev.gn`** (`devel`): workstation iteration; `devtools_skip_typecheck = true`.
- **`args/asan.gn`** (`devel`): `is_asan = true`, `devtools_skip_typecheck = true`.
- **`args/deck.gn`** (`qa`): the deployable build; `enable_widevine = true`, `use_thin_lto = false`.
- **Releases** (`scripts/release.sh`): `gold` config + the deck overrides with `use_thin_lto = true`.

Codec/legal note: `proprietary_codecs` ships H.264/AAC decoders — fine for GitHub releases; for Flathub we align with how Flathub's own Chromium handles it (runtime ffmpeg-full / openh264 extension) during P8. YouTube primary formats are VP9+Opus, so this is a compatibility fallback, not the main path.

---

## 14. Docker build & development environment

Rationale: Chromium's dependency scripts target the Debian/Ubuntu family — a container ends that fight, keeps builds reproducible, and keeps the multi-GB toolchain off the host rootfs. Source stays on the host (bind mount); the image contains only toolchain + deps.

### 14.1 Image design (`docker/Dockerfile`)
- **Base: Debian 12 (bookworm)**, matching Cobalt trunk, which builds every image on `marketplace.gcr.io/google/debian12` (set `BASE` to that digest for byte-identical parity). The hermetic Chromium toolchain needs glibc — no Alpine/musl.
- **Package lists are mirrored** from `cobalt/docker/{linux,unittest}` at the pinned commit; re-sync them on every bump (`.internal/MIGRATION.md`).
- **Stages:** `build` (engine toolchain: depot_tools with self-update off, sccache), `test` (Xvfb + media stack for `just smoke` and headless tests), `dev` (interactive tools on top), and `pack` (flatpak-builder on `debian:13-slim` — the one non-bookworm stage: flatpak-builder compiles inside the SDK sandbox, so its host distro never reaches the artifact, and bookworm's flatpak cannot express `--device=input`; `findings/durable/packaging-toolchain.md`).
- **Source is never baked in.** The gclient checkout (`/src`) and the repo (`/work`) are bind-mounted; the image rebuilds only when deps change.
- Non-root `dev` user mapped to the host UID/GID (bind-mount permission sanity).
- `docker/steamos.Dockerfile` is the separate GPU-independent test simulator (HARNESS §2.1).

### 14.2 Compose
`docker-compose.yml` defines `dev` and `pack`. `dev` mounts `${COBALT_SRC}` → `/src`, the repo →
`/work`, and the sccache cache (`SCCACHE_HOST_DIR`) → `/cache/sccache`; it uses
`network_mode: host` (deploy/rsync and remote-debug tunnels from inside) and `shm_size: 2g`
(Chromium wants a real `/dev/shm` for smoke runs). Scripts enter it through `in_container`
(`scripts/lib.sh`), which also handles rootless Docker.

**Podman:** set `CONTAINER_ENGINE=podman` in `.env`; prefer `--userns=keep-id` over the UID/GID args. Scripts only assume a `docker`-compatible CLI.

### 14.3 CI reuse
Hosted (free) runners run only what fits: `just preflight` (shellcheck, the harness suite, clang-format, launcher gcc+clang builds, gn-args) and the Pages publish. Engine builds run on the workstation in the same image with the shared sccache cache. A self-hosted runner reusing image and cache is planned, not built (TEST-PLAN §7).

---

## 15. Development scripts & tooling

Command runner: **`just`** (single static binary, self-documenting via `just --list`). Every recipe delegates to `scripts/*.sh` so CI can call scripts directly.

### 15.1 Recipe map

The full, current recipe table — what each recipe does, where it runs, whether it needs a human, and
its exit codes — is `.internal/HARNESS.md` §3. Design notes that live here:

- **`just patch-new <name>` / `just patch-refresh`** — git-based patch flow: commit in `cobalt/`,
  export with `git format-patch` into `patches/`, regenerate `series`. `just sync` re-applies the
  series with `git am`.
- **`just gdb`** (not built) — push a static `gdbserver` to the Deck (SteamOS ships no debugger; the
  rootfs stays untouched — it lives in `~/cobalt-yt/tools/`), attach host gdb with `set sysroot` +
  unstripped symbols + Chromium's `tools/gdb/` pretty-printers.
- **`just fmt`** — the tree's `buildtools/linux64/clang-format` over patch-touched files; `gn format`
  on any `.gn`; the pinned clang-format on `launcher/`.

### 15.2 Editor integration (clangd on a Chromium-sized tree)
- `compile_commands.json` from `just compdb`; `.clangd` checked in:
  ```yaml
  CompileFlags:
    CompilationDatabase: out/dev
  Index:
    Background: Skip   # flip to Build only on a >=64 GB machine for whole-tree xref
  ```
- Suggested clangd flags: `clangd --compile-commands-dir=out/dev --header-insertion=never --pch-storage=memory -j=8 --limit-results=200`.
  With `Background: Skip`, open-file indexing stays snappy even on the full tree; the launcher keeps background indexing on via its own `launcher/.clangd`.
- clangd can run inside the `dev` container against the mounted tree if you don't want a Debian-matched clangd on the host.

### 15.3 Debugging & profiling arsenal
- **First line:** Chrome DevTools over the SSH tunnel — JS/DOM/network *and* the Performance/Media panels (frame drops, decoder in use).
- **Native:** the gdbserver flow above (not built); `rr` on the workstation for nasty input/lifecycle races (record under Xvfb; not on the Deck).
- **Tracing:** Perfetto/`chrome://tracing` categories (`media`, `gpu`, `input`, `power`) captured via DevTools protocol remotely — preferred over `perf` on the Deck (no debugger/toolchain footprint on SteamOS needed).
- **Watchpoints on regressions:** `just smoke` screenshot diffs catch Leanback server-side UI shifts early (R1/R2 canary against production youtube.com/tv; a nightly run is planned).

### 15.4 Repo hygiene
- `.githooks/pre-push` runs `just preflight` (the same gate CI runs); `just hooks` enables it once per clone.
- `DEPS.pin` is the single source of truth for the Cobalt commit — bumping it is a reviewed PR that must pass `just smoke` on both presets.

---

## 16. Open questions for you
1. Build infra: the workstation as CI runner, or a rented large runner?
2. Beta channel: GitHub only, or later a Flathub `beta` branch?
