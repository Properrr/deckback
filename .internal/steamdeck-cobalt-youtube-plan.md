# Project Plan: Native YouTube (Leanback) Client for Steam Deck
**Engine:** Chromium-based Cobalt ("Chrobalt", official youtube/cobalt trunk)
**Target:** Steam Deck **OLED (Sephiroth)** — verified; **LCD (Van Gogh)** — best-effort, untested (no unit; R11). SteamOS 3.x, **Game Mode only**
**Distribution:** GitHub releases → Flathub (after test phase)
**Status:** v1 plan — 2026-07-07

---

## 1. Locked decisions & working assumptions

| # | Decision | Value | Notes |
|---|----------|-------|-------|
| D1 | Engine base | Chromium-based Cobalt (trunk / 26+) | Legacy 25 LTS Starboard path rejected (sunset). Trunk is Chromium M114-based; no public docs yet — GitHub Actions workflows in `youtube/cobalt` are the de-facto build reference. |
| D2 | Content scope | Free YouTube + best-effort Widevine (L3) | Widevine CE CDM is partner-licensed → unavailable. Realistic path: Chromium **desktop** CDM (`libwidevinecdm.so`), fetched user-side at first run (Flathub Chromium pattern). L3 caps DRM'd content resolution; free YT content is unaffected. |
| D3 | Runtime | Game Mode (gamescope) only | App runs as XWayland client under gamescope. Desktop Mode may incidentally work but is untested/unsupported in v1. |
| A1 | HW video decode | Software decode ships; VA-API was Phase 4. **Update 2026-07-10: BLOCKED** — VA-API engaged but rendered green-band corruption on every frame; reverted, ships software decode (findings/milestones/m114.md). | Battery/thermals still argue for hw decode at "1.0", but it needs a Mesa/ANGLE-layer fix gated on a pixel check. |
| A2 | Branding | **Deckback** (app id `io.github.properrr.deckback`) | Cannot use "YouTube" in the app name or ship YouTube logos. |
| A3 | Ad blocking / UI mods | **None in v1** | Keeps Flathub acceptance and takedown risk low. Revisit later as opt-in if desired. |
| A4 | Build host | Your workstation (or CI), **not** the Deck | Measured footprint ≈45 GB (source + one preset + caches), 32 GB+ RAM recommended. Deck is deploy/test target over SSH only. |

---

## 2. Goals / non-goals

**Goals**
- Console-grade YouTube TV (Leanback) experience in Game Mode, on par with PS5's app: full controller navigation, in-UI on-screen keyboard, voice search via built-in mic, seek/skip on shoulder buttons, correct back-button semantics.
- Robust sleep/resume: playback pauses on suspend, network and player recover cleanly on wake.
- Idle/dim inhibition while video plays.
- Hardware decode (VP9/H.264/HEVC via VCN) for battery life; AV1 explicitly steered away. **NB:** the old parenthetical "no AV1 hw decode on Van Gogh **or** Sephiroth" is **disputed and must not be repeated** — libva on the OLED advertises `AV1Profile0` under `VAEntrypointVLD` (`findings/durable/hardware.md`). Steering stays on because it is free and the LCD is unmeasurable, not because the hardware is known to lack AV1.
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
│ │  │  │ launcher (thin C++/sh shim)                 │  │  │ │
│ │  │  │  · session/env setup, UA config, flags      │  │  │ │
│ │  │  │  · logind sleep-watcher (D-Bus)             │  │  │ │
│ │  │  │  · idle-inhibit manager                     │  │  │ │
│ │  │  │  ┌───────────────────────────────────────┐  │  │  │ │
│ │  │  │  │ Cobalt (Chromium M114 content shell)  │  │  │  │ │
│ │  │  │  │  · loads https://www.youtube.com/tv   │  │  │  │ │
│ │  │  │  │  · Blink/V8, net stack (HTTP/3)       │  │  │  │ │
│ │  │  │  │  · input layer: evdev gamepad →       │  │  │  │ │
│ │  │  │  │    Leanback key events (C++ side)     │  │  │  │ │
│ │  │  │  │  · media: FFmpeg sw → VA-API hw       │  │  │  │ │
│ │  │  │  │  · audio: PipeWire (pulse compat)     │  │  │  │ │
│ │  │  │  │  · mic: getUserMedia → PipeWire       │  │  │  │ │
│ │  │  │  │  · CDM host → libwidevinecdm.so (L3)  │  │  │  │ │
│ │  │  │  └───────────────────────────────────────┘  │  │  │ │
│ │  │  └─────────────────────────────────────────────┘  │  │ │
│ │  └── X11 (XWayland) or Ozone/Wayland — decided in P1 ─┘  │ │
│ └─────────────────────────────────────────────────────────┘ │
│  Steam Input (non-Steam shortcut) — shipped config template │
│  systemd-logind (suspend signals) · PipeWire · Mesa/RADV    │
└─────────────────────────────────────────────────────────────┘
```

**Key architectural bets**
1. **Identity:** ship a console-class UA (`Mozilla/5.0 (X11; Linux x86_64; Leanback Shell) Cobalt/26...`-style, config-driven, hot-swappable via a JSON config) so Leanback serves the TV app and enables its console feature set. This is the single biggest external dependency/risk (§11-R1).
2. **Input at the browser layer, not JS injection:** translate evdev/SDL gamepad state into `ui::KeyEvent`s inside the Cobalt embedder (VacuumTube does this with injected JS in Electron; we can do it natively in C++ — lower latency, works before page JS loads, survives page navigation). **[Landed 2026-07-10 patch-free:** input shipped as evdev→CDP `Input.dispatchKeyEvent` injection in `launcher/`, not an in-tree `ui::KeyEvent` patch — CDP events arrive `isTrusted`, so no engine patch was needed (m114.md).]**
3. **Two-level input:** in-app native handling is primary; a shipped Steam Input template ("Gamepad with deck-specific extras") is the transport. Trackpad-as-pointer passes through unmodified; **the touchscreen is made inert by default** (`disable_touch`, verified 2026-07-10) — see `findings/durable/touch-lock.md` (the earlier EVIOCGRAB lock is proven dead).

---

## 4. Hardware/platform matrix

> **⚠ Locked 2026-07-09: there is no LCD unit and none is planned** (`findings/durable/decisions.md`).
> Every "test both" below is unexecutable. **LCD is best-effort and untested for v1**, not supported.
> Code stays model-agnostic; only our *claims* narrow. See `findings/durable/hardware.md` for which
> facts transfer and which do not.

| Aspect | LCD (Van Gogh) — **untested** | OLED (Sephiroth) — our only unit | Plan impact |
|---|---|---|---|
| APU | Zen2 4c/8t + RDNA2 8CU, 7nm | Same arch, 6nm | Identical code path |
| Video decode (VCN) | H.264, HEVC, VP9 ✔ / AV1 **⚠ disputed, now unmeasurable** | Same, but AV1 **measured present** (libva `AV1Profile0`/VLD) | Steer YT to VP9/H.264 (`steer_av1_unsupported`, launcher-side). **Stays default-on permanently** — an OLED-only result cannot license AV1 for untestable LCDs |
| Display | 1280×800 **60 Hz** | 1280×800 90 Hz, HDR | Render 1280×720 letterboxed; cap 60 fps. The 90→60→45 Hz QAM sweep is **OLED-only** — meaningless on a 60 Hz panel |
| Wi-Fi | RTL8822CE (Wi-Fi 5) | QCNFA765 (Wi-Fi 6E) | Resume/reconnect timing differs. Only the OLED chip can be tuned → keep the resume backoff conservative, not fitted to one chip |
| Mic | Built-in dual mic array | Built-in dual mic array | P5 verifies OLED only |
| Touchscreen | id **unknown** | id **unverified** (`2808:1015` is from an LCD blog post, never measured here) | `touch.cpp` pins that id; its name/capability fallback is the real cross-model path and is untested. Dump the OLED's true id first |
| Power | 7 nm — draws more | 6 nm — the ≤9 W target is measured here | **Never restate OLED battery numbers as LCD numbers** |
| RAM | 16 GB shared | 16 GB shared | Chromium footprint fine |

---

## 5. Development workflow (build host + Deck over SSH)

*(Toolchain details: §13 · container environment: §14 · scripts: §15.)*

**Build host (your machine):**
- `depot_tools`, `gclient` checkout of `youtube/cobalt` trunk. Follow the repo's GitHub Actions Linux workflow as the build recipe (no public docs exist for Chrobalt — the CI YAML *is* the documentation).
- `gn` args: start from the Actions linux config; add `cc_wrapper="sccache"` (Cobalt team's own recommendation for meaningful iteration).
- Expect: ~45 GB disk (measured — source + one preset + caches), first build 1–3 h on a 16-core box, incrementals fine with sccache.

**Deck (deploy target, once you hand me SSH):**
- Prereqs on Deck: Developer Mode on, `sshd` enabled, password set for `deck` user. **No** read-only filesystem changes needed — everything lives in `~/` or Flatpak.
- Iteration loop:
  1. `./scripts/deploy.sh deck@<ip>` — rsync stripped build → `~/cobalt-yt/` on Deck.
  2. Run from Desktop-mode konsole first (fast sanity), then via non-Steam shortcut in Game Mode.
  3. Remote debugging: run with `--remote-debugging-port=9222`, `ssh -L 9222:localhost:9222`, attach Chrome DevTools from the workstation (`chrome://inspect`).
  4. Logs: journald + app log dir; `./scripts/logs.sh` tails over SSH.
- Add-to-Steam automation: `steamos-add-to-steam` (present on SteamOS) or direct `shortcuts.vdf` manipulation; script sets launch options, compat off, and points artwork.

**What I'll need from you at implementation time:** Deck IP + SSH credentials, which unit (LCD/OLED) is first, whether the build runs on your workstation with me driving via SSH there too, or you build and I only get the Deck.

---

## 6. Phased implementation plan

Estimates are focused engineering days (ED) for one senior engineer; calendar time depends on your availability. Total ≈ **45–60 ED** to Flathub submission.

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

Deliverable: Phase 0 spike findings — landed in `.internal/findings/milestones/m114.md` (not the
originally-planned `docs/spikes.md`); go/no-go on any plan adjustments.

### Phase 1 — Build & bring-up on Deck (5–7 ED)
- Reproducible build: pin a trunk commit; `build/` dir with gn args files (debug/release), sccache, `deploy.sh`.
- First run on Deck in Desktop Mode (X11), then Game Mode as non-Steam shortcut.
- Resolution/aspect: force 1280×720 render surface, gamescope letterboxes to 800p. Verify text sharpness (Leanback is authored for 1080p — check scaling quality, consider 1920×1080 render + gamescope downscale if GPU headroom allows).
- Persistent profile dir (`~/.var/app/...` shaped even pre-Flatpak) so sign-in survives updates.
- Crash handling: crashpad on, local minidumps only (no telemetry).
- **Acceptance:** cold boot → Game Mode → launch → browse with touchscreen → play 1080p video with audio, stable 30 min.

### Phase 2 — Session & lifecycle plumbing (3–4 ED)
- Launcher shim (small C++ or POSIX sh) owning: env, flags, config file (UA, URL, quality caps), log rotation, watchdog restart-on-crash (Leanback expects app restart semantics like a TV).
- Single-instance lock; deep-link support (`app://` arg → `youtube.com/tv#?v=` for future "open video" integrations).
- Clean exit path: map a controller chord + Leanback's own Exit menu item → proper shutdown (Steam overlay "Close game" must also not corrupt the profile).
- **Acceptance:** 50 launch/kill cycles via Steam UI with zero profile corruption.

### Phase 3 — Input: PS5-parity controls (6–8 ED)
Primary mechanism (from S0.6): either (a) Blink Gamepad API consumed by Leanback natively under console UA — then we only need mapping/config; or (b) our own evdev→`ui::KeyEvent` translation layer in the embedder. Plan assumes (b) as the safe default; (a) reduces scope. **[Actual 2026-07-10:** neither — input landed as evdev→CDP key injection in the launcher, patch-free (m114.md).]**

**Mapping (default template, PS5-style):**

| Deck control | Action | Transport |
|---|---|---|
| D-pad / Left stick | Navigate (arrow keys) | key events, auto-repeat with accel |
| A | Select / OK (Enter) | key |
| B | Back (Esc / Leanback BACK) | key |
| X | Play/Pause | media key |
| Y (**hold**) | Voice search — **NOT a key** (findings input-ux §8.2/§13). `kSbKeyMicrophone` does nothing: Cobalt routes voice through a Starboard service this build lacks | **hold-to-talk** → trusted `Input.dispatchMouseEvent` on Leanback's soft-mic button; duck playback while capturing |
| LB / RB | Scrub back / forward (Left/Right in player) — **not** a fixed ±10 s jump; Leanback publishes no fixed-interval seek key, so we don't guess one (resolved, findings input-ux §12) | Left/Right in player |
| LT / RT (full pull) | Scan rewind / fast-forward (hold) | `MediaRewind` 227 / `MediaFastForward` 228 (VK, *unverified* — spike first) |
| ☰ Start | Player menu / settings | key |
| ⧉ Select | Toggle captions | key (Leanback `C`) |
| Right trackpad | Pointer + click (optional, off by default) | mouse |
| Left trackpad | Scroll lists | wheel |
| Touchscreen | **Inert by default** (`disable_touch`, 2026-07-10; EVIOCGRAB lock proven dead — see touch-lock.md) | `no_pointer.js` swallow + gamescope hover |
| L4/R4/L5/R5 | Unbound (user-assignable) | — |
| Steam / QAM | Reserved by Steam | — |

- On-screen keyboard: use **Leanback's own OSK** (search, sign-in) — d-pad navigable like PS5.
  **Corrected 2026-07-09 (findings input-ux §8.3):** Steam's OSK is *not* a safe fallback as written.
  (a) `STEAM+X` on a non-Steam Chromium shortcut can raise the **desktop** OSK and **soft-lock** the
  session — the reliable path is **QAM ("…") → keyboard**. (b) Under `--ozone-platform=x11` the OSK
  **never auto-appears on `<input>` focus** (that needs Wayland `text-input-v3`), and we cannot call
  the Steamworks OSK API. So Valve's "auto-invoke an OSK for text input" recommendation is
  **structurally unmeetable** — document it, don't fake it. Physical/BT keyboards do reach the app,
  but Game Mode forces **QWERTY scancodes** regardless of layout.
- Keycodes: Chrobalt is Chromium → `keyCode` is the **Windows VK** set (`SbKey` == VK). The
  CEA-2014/HbbTV codes (461 GoBack, 415 Play, …) are a **different keycode world** (Tizen/webOS) and
  are not native here; desktop watch-page shortcuts (`j`/`l`/`k`/`/`) are a **third** surface. Google
  publishes no key contract for the TV app — bind nothing without an on-Deck spike (findings §8.1/§8.4).
- Ship a Steam Input template (`config/steam_input.vdf`): plain "Gamepad" layout so in-app handling sees raw pad; document how users apply it (non-Steam apps can't push official layouts — provide import instructions + community layout name).
- Rumble: light haptic tick on focus move via SDL rumble (config-off by default) — PS5-ish flourish, cheap once evdev layer exists.
- **Acceptance:** every Leanback surface (home, search, player, settings, sign-in) fully drivable with controller only; input latency subjectively ≤ PS5 app; OSK usable end-to-end.

### Phase 4 — Media pipeline & hardware decode (8–12 ED)
> **Status 2026-07-10:** Step 2 shipped (patch-free, in the launcher). Step 3 (VA-API) is **BLOCKED
> and reverted** — it engaged the hardware decoder but painted green-band corruption on every frame
> while every automated metric false-passed; a pixel gate now guards any re-enable. Deckback ships
> **software decode**. See findings/milestones/m114.md §"VA-API decode is VISUALLY CORRUPT".
- **Step 1 (sw decode hardening):** confirm VP9 1080p60 software decode headroom; set default max quality 1080p (800p panel) via player settings persistence; measure battery draw baseline.
- **Step 2 (format steering):** shape `MediaCapabilities`/`canPlayType` responses: report AV1 unsupported; report VP9/H.264 supported so YouTube's ABR picks hw-friendly streams. **Landed patch-free** in `launcher/src/navigator.cpp` via CDP script injection (`av1_steering.js`) — not the "surgical Blink patch" originally planned.
- **Step 3 (VA-API) — BLOCKED (see status note above):** enabling `VaapiVideoDecoder` (`--enable-features=VaapiVideoDecodeLinuxGL --use-angle=gl`) against Mesa radeonsi selected the hw decoder but ANGLE's DMA-buf import of the tiled NV12 VA surface is broken → corruption. Reverted; re-enable requires a Mesa/ANGLE fix and a **pixel** check (not a decoder-name check).
- **Step 4 (power validation):** target: 1080p VP9 playback ≤ ~8–9 W total system draw (vs ~12–15 W sw decode). Measure via `/sys/class/power_supply`.
- **Acceptance:** 4-hour continuous playback on battery from full charge (OLED), no thermal throttling artifacts, no A/V desync after seek storms.

### Phase 5 — Audio & microphone (3–5 ED)
- Audio out: PipeWire (pulse compat layer is fine for Chromium M114). Verify: speaker, 3.5 mm, Bluetooth (SBC/AAC), and seamless device switching mid-playback.
- Mic in: `getUserMedia` → PipeWire source. Auto-grant mic permission for `youtube.com` origin only (embedder-level permission policy — no permission prompts exist in a kiosk shell).
- Voice search e2e: hold-to-talk → Leanback mic UI → speech recognized (YouTube's server-side ASR) → results. OLED mic array + BT headset mics (no LCD unit). **Not a key press** — see §P3 and findings input-ux §8.2.
- Suspend interaction: PipeWire nodes disappear on suspend — ensure graceful stream rebuild on resume (Phase 6 hook).
- **Acceptance:** voice search success in a normal room on both LCD and OLED; audio device hot-swap without app restart.

### Phase 6 — Power: sleep, resume, idle inhibit (4–6 ED)
- **Suspend:** subscribe to `org.freedesktop.login1.Manager.PrepareForSleep` on system D-Bus; take a *delay* inhibitor lock → on `true`: pause player (synthesize Pause), checkpoint position, flush profile → release lock → system sleeps.
- **Resume:** on `PrepareForSleep(false)`: wait for network-online (NetworkManager D-Bus state or simple reachability probe with backoff — expect different latencies on RTL8822CE vs QCNFA765), rebuild PipeWire streams, nudge player (Leanback recovers position; verify token refresh for long sleeps > session TTL).
- **Idle/dim inhibit while playing:** hold `org.freedesktop.ScreenSaver` / logind idle inhibitor while player state == playing; release on pause. **Known risk:** Game Mode's dim/suspend timer may ignore standard inhibitors for non-Steam apps — spike it; fallback: synthetic zero-motion input event every 30 s while playing (documented, behind config flag).
- **Acceptance:** 25× scripted suspend/resume loop mid-playback: app alive, position correct, audio back, no zombie network state; screen never dims during a 30-min video; screen dims normally when paused.

### Phase 7 — Widevine path (4–6 ED, best-effort by design)
- From S0.3: re-enable `enable_library_cdms=true enable_widevine=true` if stripped; implement CDM host registration for the desktop CDM.
- First-run fetcher (in launcher, not the Flatpak): download Google's official Linux x64 CDM from Google's own endpoint (the Flathub Chromium extra-data pattern), verify hash, install to profile dir. **Never redistributed by us.**
- Expectations set honestly: L3 software CDM → DRM'd content (rentals, some live/originals) plays but is resolution-capped by YouTube policy (typically ≤ 480/720p); free content unaffected. If YouTube requires VMP for rentals, feature is documented as unsupported — free-YT experience is the product.
- **Acceptance:** a known Widevine test stream + one YT movie rental plays; graceful, human-readable error if CDM missing/rejected.

### Phase 8 — Packaging & distribution (5–7 ED)
- **Flatpak:** `org.freedesktop.Platform//25.08` (shipped; was 24.08 in the original plan); Chromium sandbox inside Flatpak via **zypak** (Flathub-standard; no setuid, no nested userns). Fallback decision point: `--no-sandbox` is *not* acceptable for Flathub — zypak is the plan of record.
- Permissions (minimal): `--share=network`, `--socket=x11` (or wayland per P1 outcome), `--socket=pulseaudio` (PipeWire), `--device=input` (evdev gamepads; avoid `--device=all`), `--talk-name=org.freedesktop.ScreenSaver`, system-bus talk to `org.freedesktop.login1`.
- Widevine as `extra-data` or first-run fetch (legal review note in repo).
- **GitHub:** repo layout (§9), Releases with `.flatpak` bundle + flatpakref; CI reality check: Chromium-scale builds do **not** fit free GitHub runners (disk/RAM). Plan: self-hosted runner on your workstation (or a rented large runner) doing weekly + tag builds with persistent sccache volume; free runners handle lint/scripts/docs only.
- **Add-to-Steam UX:** `just install` / script: installs Flatpak, runs `steamos-add-to-steam`, prints controller-layout import steps, sets grid artwork (original, non-infringing art).
- **Acceptance:** clean SteamOS **OLED** unit: install from GitHub release → playing a video in Game Mode in < 5 minutes, no terminal knowledge beyond one command. (LCD: best-effort, untested — R11.)

### Phase 9 — Test & hardening pass (5–7 ED)
Matrix (**OLED only** — no LCD unit exists, R11 / decisions.md):

| Area | Tests |
|---|---|
| Playback | VP9/H.264 hw + sw, 60 fps content, live streams, seek storm, 4 h soak, BT audio |
| Input | Full-surface controller traversal, OSK, touch, external BT pad, layout re-import |
| Voice | Quiet/noisy room, BT mic, mid-playback search |
| Power | 25× suspend loop, overnight sleep, idle-inhibit, low-battery behavior |
| Network | Wi-Fi drop/roam mid-stream, captive-ish latency, resume-after-sleep reconnect |
| Account | Sign-in (on-device + code pairing), sign-out, multi-account, token expiry after 3-day sleep |
| DRM | Widevine clip, rental (expected caps documented) |
| Update | Flatpak upgrade in place: profile, sign-in, settings survive |
| Docked (smoke only) | HDMI 1080p output sanity — not a v1 feature, must not crash |

- Fix window + a small beta group (r/SteamDeck etc.) before Flathub.

### Phase 10 — Flathub submission & maintenance (3–4 ED + ongoing)
- Flathub review: precedent exists (VacuumTube is on Flathub), but our build is a full Chromium derivative — expect scrutiny on sandbox (zypak), extra-data (Widevine), and trademark (name/icon per §12).
- Maintenance posture: track `youtube/cobalt` trunk monthly; Leanback is server-side and *will* change under us — keep UA and key-map in a remotely-overridable config (fetched from our GitHub raw, signed) so breakage can be hotfixed without a 2-hour rebuild.
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
| R2 | Chrobalt trunk instability / no docs / breaking churn | High | Delays | Pin commits; treat CI YAML as docs; keep our patches as a thin, rebaseable quilt series in `patches/`; monthly rebase cadence. |
| R3 | VA-API on M114 needs heavy backports | Med | Battery goal slips | Ship sw-decode M1; steer formats (biggest win) independently of hw decode. |
| R4 | Game Mode ignores idle inhibitors for non-Steam apps | Med | Annoying UX | Synthetic-activity fallback (flagged), file issue with Valve, document. |
| R5 | Widevine desktop CDM rejected by YT for rentals (VMP) | Med | Feature degraded | Scope framed as best-effort; free YT is the product. |
| R6 | Trademark complaint (name/branding) | Low-Med | Rename/relist | §12 rules: no "YouTube" in name, no logo, "unofficial" in description. |
| R7 | CI cost/scale for Chromium builds | High | Slow releases | Self-hosted runner + sccache; release cadence monthly, hotfixes via config channel (no rebuild). |
| R8 | Flathub rejects zypak'd custom Chromium | Low | GitHub-only distribution | GitHub flatpakref remains primary until resolved; engage reviewers early with manifest draft. |
| R9 | **Text entry is a dead end**: Leanback's grid OSK may not accept printable keys, the Steam OSK never auto-appears under Xwayland, and `STEAM+X` can soft-lock a non-Steam Chromium app | Med | Search/sign-in painful or unusable | Spike printable typing first (findings input-ux §8.4 — it decides everything downstream); document QAM-not-STEAM+X; Steam Input keyboard binds as fallback; native-Wayland/`text-input-v3` as the stretch fix (§10.7). |
| R10 | **Most of the build is unverified on hardware** — L2 automation exists but ran against a Deck for the first time only 2026-07-10, and is still blocked by Leanback's guest-account gate (harness.md F14) | High | False confidence | On-Deck harness built (`just test-deck`); `.internal/TEST-PLAN.md` §2 keeps an honest tested/untested matrix; an account-gate fixture is the next unlock. |
| R11 | **No LCD unit exists** (locked 2026-07-09) — the LCD can never be verified in-house | Certain | LCD users hit untested paths; AV1 dispute permanently open | Ship LCD as **best-effort/untested**, stated in README + SUPPORT.md; keep `steer_av1_unsupported` default-on forever; keep resume backoff conservative rather than fitted to the one Wi-Fi chip we can measure; never publish OLED power numbers as LCD numbers; **recruit an LCD owner for the P9 beta** — the only realistic coverage path, and a scheduling dependency. |

---

## 9. Repository layout

```
<name>/
├── cobalt/                  # gclient-managed checkout (gitignored), pinned via DEPS.pin
├── patches/                 # quilt series against pinned cobalt commit
│   └── 0001-...-widevine-cdm-registration.patch   # the ONLY patch — input/steering/mic
│                            #   all landed patch-free in the launcher over CDP, as it turned out
├── launcher/                # shim: config, sleep-watcher, idle-inhibit, CDM fetcher, watchdog
├── config/
│   ├── app.json             # UA, URL, quality caps, keymap (remotely overridable, signed)
│   ├── av1_steering.js      # injected: report AV1 unsupported
│   ├── no_pointer.js        # injected: make touch inert (disable_touch)
│   └── steam_input.vdf      # controller template
├── flatpak/<app.id>.yml
├── scripts/                 # deploy.sh, logs.sh, add-to-steam.sh, bench-power.sh
├── docs/                    # public deliverables: HOW-IT-WORKS.md, SUPPORT.md, legal.md
│                            #   (spike findings live in .internal/findings/, not docs/)
└── .github/workflows/       # lint (hosted), build+release (self-hosted)
```

---

## 10. Stretch goals (post-1.0 backlog)
1. **HDR on OLED** — gamescope HDR + Chromium HDR video on Linux is bleeding-edge; revisit when Chrobalt rebases past M114.
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

## 11. Timeline sketch (single engineer, ~60% allocation)

| Weeks | Milestone |
|---|---|
| 1–2 | P0 spikes complete, plan v2 adjustments |
| 3–5 | M1: video playing in Game Mode, touch-driven (P1–P2) |
| 6–8 | M2: full controller parity + audio/mic (P3, P5) |
| 9–12 | M3: hw decode + sleep/power solid (P4, P6) |
| 13–14 | M4: Widevine best-effort + Flatpak beta on GitHub (P7, P8) |
| 15–17 | M5: QA matrix green on OLED, beta feedback incl. a recruited LCD owner (P9) |
| 18+ | Flathub submission (P10) |

---

## 12. Naming & trademark guardrails
Rules: no "YouTube"/"YT" in the app id or store name; no red play-button or logo derivatives; description states "unofficial client for YouTube's TV interface, built on the open-source Cobalt engine"; app id like `io.github.<you>.<name>`.

Proposals: **Deckback** (Deck + Leanback — my pick), **Leandeck**, **Cobalt Theater**, **Redshift Player** (maybe too astronomy-loaded). Your call, or bring your own.

---

## 13. Toolchain: compilers & C++ standards

### 13.1 In-tree (Cobalt/Chromium patches) — the toolchain chooses you
- **Compiler:** Chromium's **hermetic, bundled Clang** — non-negotiable. `gclient sync` hooks fetch a pinned toolchain into `third_party/llvm-build/Release+Asserts/`; the M114 base pins roughly Clang 16/17. GCC is **not supported** by the Chromium build system (distro maintainers carry large patch sets to force it — we won't). Do not substitute a system Clang either: version skew breaks `-Werror`, plugins, and PGO/LTO assumptions.
- **Linker:** bundled **lld** (default). **Stdlib:** Chromium's in-tree **libc++** (`use_custom_libcxx=true`, default) — all in-tree patches compile against it, not the system stdlib.
- **C++ standard: C++17** for anything under `patches/`. M114 predates Chromium's C++20 migration; leaking C++20 into patches will both fail the toolchain gate and maximize rebase pain when Chrobalt moves to a newer milestone (at which point we bump). Chromium style guide applies; format with the tree's own `buildtools/linux64/clang-format` (see `just fmt`).
- **Sanitizers:** `is_asan=true` builds for workstation-side repro of memory bugs; not deployable to the Deck (perf/size). TSan rarely needed for our patch surface.

### 13.2 Out-of-tree (launcher, CDM fetcher, tools) — our choice
- **Standard: C++23.** Small, modern, self-contained codebase; no reason to stay on 20.
- **Compilers: dual-clean.** Primary dev with current stable **Clang (≥18)**; must also build with **GCC 14**, because the Flatpak build uses the `org.freedesktop.Sdk//25.08` toolchain (GCC-based). CI compiles both, `-Wall -Wextra -Werror`, ASan/UBSan job on Clang.
- **Build:** CMake ≥ 3.28 + Ninja. Deliberately *not* folded into the GN tree — the launcher rebuilds in seconds and must never require a Chromium checkout to hack on.
- **Deps (minimal by design):** `libsystemd` (sd-bus for logind/ScreenSaver — systemd is guaranteed on SteamOS), `libevdev` (input layer prototyping/tools), `libcurl` (CDM fetcher), stdlib otherwise. No Boost/Folly-class deps.

### 13.3 GN args presets (checked into `args/`, exact surface confirmed in S0.1)

**`args/dev.gn` — workstation iteration:**
```gn
target_os = "linux"
target_cpu = "x64"
is_debug = false           # "release + DCHECKs" iterates far faster than full debug
dcheck_always_on = true
is_component_build = true  # shared-lib build: ~10x faster links
symbol_level = 1
blink_symbol_level = 0
v8_symbol_level = 0
cc_wrapper = "sccache"
proprietary_codecs = true
ffmpeg_branding = "Chrome" # H.264/AAC/HEVC demux+decode; without this YT fallback formats break
```

**`args/deck.gn` — deployable static build:**
```gn
target_os = "linux"
target_cpu = "x64"
is_debug = false
is_component_build = false
is_official_build = true   # full opts; if Chrobalt gates this on internal bits, fall back to manual opt flags
symbol_level = 1           # symbols kept host-side; binary deployed stripped
proprietary_codecs = true
ffmpeg_branding = "Chrome"
use_thin_lto = false       # true for tagged releases only (link-time cost)
cc_wrapper = "sccache"
```

**`args/asan.gn`:** `dev.gn` + `is_asan = true`.

Codec/legal note: `proprietary_codecs` ships H.264/AAC decoders — fine for GitHub releases; for Flathub we align with how Flathub's own Chromium handles it (runtime ffmpeg-full / openh264 extension) during P8. YouTube primary formats are VP9+Opus, so this is a compatibility fallback, not the main path.

---

## 14. Docker build & development environment

Rationale: your workstation is CachyOS/Arch while Chromium's dependency scripts target the Debian/Ubuntu family — a container ends that fight permanently, makes CI byte-identical to local builds, and keeps the multi-GB toolchain off the host rootfs. Source stays on the host (bind mount); the image contains only toolchain + deps.

### 14.1 Image design (multi-stage)

> **[Updated 2026-07-10:** the real build image is **Debian 12 (bookworm)**, matching Cobalt trunk
> (which builds on `marketplace.gcr.io/google/debian12`), not Ubuntu 22.04; the `pack` stage alone is
> `debian:13-slim`. See `findings/durable/packaging-toolchain.md` / `decisions.md`. The Dockerfile
> below is illustrative of the structure, not the exact base.]**

```dockerfile
# syntax=docker/dockerfile:1.7
# NOTE: the shipped docker/Dockerfile builds on Debian 12 (bookworm) to match Cobalt trunk's own
# images — not ubuntu:22.04 as sketched here. The `pack` stage alone uses debian:13-slim.
FROM debian:12 AS build
ENV DEBIAN_FRONTEND=noninteractive LANG=en_US.UTF-8
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl git python3 python3-pip lsb-release sudo \
      locales xz-utils zip unzip pkg-config file rsync openssh-client \
    && locale-gen en_US.UTF-8 && rm -rf /var/lib/apt/lists/*

# depot_tools (gclient, gn, autoninja); self-update disabled for hermeticity
RUN git clone --depth=1 \
      https://chromium.googlesource.com/chromium/tools/depot_tools /opt/depot_tools
ENV PATH=/opt/depot_tools:$PATH DEPOT_TOOLS_UPDATE=0

# Chromium build deps, pinned to the exact Cobalt commit we build (DEPS.pin)
ARG COBALT_COMMIT
RUN curl -fsSL https://raw.githubusercontent.com/youtube/cobalt/${COBALT_COMMIT}/build/install-build-deps.sh \
      -o /tmp/ibd.sh \
 && bash /tmp/ibd.sh --no-prompt --no-arm --no-nacl \
 && rm -rf /var/lib/apt/lists/*

# sccache — persistent compile cache on a named volume
ARG SCCACHE_VER=v0.10.0
RUN curl -fsSL https://github.com/mozilla/sccache/releases/download/${SCCACHE_VER}/sccache-${SCCACHE_VER}-x86_64-unknown-linux-musl.tar.gz \
    | tar xz --strip-components=1 -C /usr/local/bin --wildcards '*/sccache'
ENV SCCACHE_DIR=/cache/sccache SCCACHE_CACHE_SIZE=100G

# non-root user mapped to host UID/GID (bind-mount permission sanity)
ARG UID=1000 GID=1000
RUN groupadd -g ${GID} dev && useradd -m -u ${UID} -g ${GID} -s /bin/bash dev \
 && echo 'dev ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/dev
USER dev
WORKDIR /src

# ---- interactive dev layer on top of the pure builder ----
FROM build AS dev
USER root
RUN apt-get update && apt-get install -y --no-install-recommends \
      gdb lldb clangd-17 quilt ripgrep fd-find jq shellcheck bash-completion \
      xvfb x11-utils strace ltrace less neovim \
 && curl -fsSL https://github.com/casey/just/releases/latest/download/just-x86_64-unknown-linux-musl.tar.gz \
    | tar xz -C /usr/local/bin just \
 && rm -rf /var/lib/apt/lists/*
USER dev

# ---- packaging layer: flatpak-builder for P8 ----
FROM dev AS pack
USER root
RUN apt-get update && apt-get install -y --no-install-recommends \
      flatpak flatpak-builder && rm -rf /var/lib/apt/lists/*
USER dev
```

Design points:
- **Source is never baked in.** `/src` is the host checkout; the image is ~3–4 GB of toolchain and rebuilds only when `DEPS.pin`/deps change.
- **`COBALT_COMMIT` build-arg** pins `install-build-deps.sh` to the same commit as the source — deps and source can't drift.
- **Three targets:** `build` (CI), `dev` (interactive, debuggers/editors), `pack` (flatpak-builder). CI uses `build` — smallest attack/maintenance surface.
- **Headless smoke test** (CI): `xvfb-run out/deck/cobalt --url=... --remote-debugging-port=9222` + a DevTools-protocol probe script asserting the Leanback app booted (title/DOM check, screenshot artifact).

### 14.2 Compose

```yaml
services:
  dev:
    build:
      context: docker/
      target: dev
      args:
        UID: "${UID:-1000}"
        GID: "${GID:-1000}"
        COBALT_COMMIT: "${COBALT_COMMIT}"   # sourced from DEPS.pin by justfile
    volumes:
      - ${COBALT_SRC:-./cobalt}:/src
      - sccache:/cache/sccache
    network_mode: host   # deploy/rsync to Deck + remote-debug tunnels from inside
    shm_size: 2g         # Chromium runtime wants real /dev/shm for smoke runs
    stdin_open: true
    tty: true
volumes:
  sccache:
```

**Podman note (Arch/CachyOS):** works rootless as-is with `podman compose`; replace the UID/GID args with `--userns=keep-id` and drop the sudoers line if you prefer. Either engine is fine — scripts only assume a `docker`-compatible CLI (configurable `CONTAINER_ENGINE` in `.env`).

### 14.3 CI reuse
The self-hosted runner (your workstation) runs jobs inside the same `build` image with the same named `sccache` volume — local builds warm CI's cache and vice versa. Weekly `sccache --show-stats` + cache GC in a maintenance job. Hosted (free) runners only run: shellcheck, `gn format --dry-run` on patches, launcher build+tests (that one fits anywhere), clang-format check.

---

## 15. Development scripts & tooling

Command runner: **`just`** (single static binary, self-documenting via `just --list`). Every recipe below delegates to `scripts/*.sh` so CI can call scripts directly.

### 15.1 Recipe map

| Recipe | Purpose / implementation notes |
|---|---|
| `just bootstrap` | One-time: build image, `gclient config` + first `sync` at `DEPS.pin` commit. Prints disk/RAM sanity warnings first. |
| `just sync` | `gclient sync -r $(cat DEPS.pin)` + re-apply `patches/` series. Refuses to run with a dirty tree. |
| `just gen <preset>` | `gn gen out/<preset> --args="$(cat args/<preset>.gn)"` inside container. |
| `just build <preset>` | `autoninja -C out/<preset> cobalt` (+ launcher via CMake). Default preset: `dev`. |
| `just compdb` | `ninja -C out/dev -t compdb cc cxx > compile_commands.json`, symlinked to repo root for clangd. |
| `just smoke` | Xvfb headless boot + DevTools-protocol assertion + screenshot (same script CI runs). |
| `just deploy [host]` | Strip binary + resources → `rsync --delete` → `~/cobalt-yt/` on Deck. Unstripped copy kept at `out/deck/symbols/` for gdb. Host from `.env` (`DECK_HOST=deck@<ip>`). |
| `just run` | SSH-launch on Deck with flags from `config/app.json`, tee to remote log file. |
| `just logs` | `ssh $DECK_HOST tail -f ...` + relevant `journalctl -f` filter. |
| `just debug` | Opens `ssh -L 9222:localhost:9222` tunnel, prints `chrome://inspect` instructions. |
| `just gdb` | Pushes a static `gdbserver` to the Deck (SteamOS ships no debugger; rootfs stays untouched — lives in `~/cobalt-yt/tools/`), attaches host gdb with `set sysroot` + unstripped symbols + Chromium's `tools/gdb/` pretty-printers. |
| `just patch-new <name>` / `just patch-refresh` | Git-based quilt flow: commit in `cobalt/`, export with `git format-patch` into `patches/`, series file regenerated. `sync` re-applies. |
| `just fmt` | Tree's `buildtools/linux64/clang-format` over patch-touched files; `gn format` on any `.gn`; clang-format (LLVM style or your pick) on `launcher/`. |
| `just power` | During playback: poll `/sys/class/power_supply/BAT*/power_now` over SSH, emit CSV → used for the P4 ≤9 W acceptance gate. |
| `just soak [n]` | n× suspend/resume loop over SSH (`rtcwake -m mem -s 45`), verifying app liveness + playback position after each cycle (P6 acceptance). |
| `just test-deck [expr]` | **(Phase T)** pytest harness on the workstation driving the Deck over SSH + CDP (+uinput). The missing L2 tier — see `.internal/TEST-PLAN.md`. |
| `just cert [suite]` | **(Phase T)** self-hosted `youtube/js_mse_eme` conformance, headless on the workstation — **no Deck needed**, runs per-PR. Unattended: reverse-tunnelled `localhost` origin (secure context ⇒ EME works; also kills the phone-home), hash-pinned vectors, no-progress watchdog, committed expectations baseline. Exit code = regression, not raw failure. |
| `just cert-deck [suite]` | **(Phase T)** the same on real hardware, adding `playbackperf-*` (a trend, never a build gate). |
| `just flatpak` | `pack` image → `flatpak-builder` → local repo + `.flatpak` bundle. |
| `just release <tag>` | deck.gn (+ThinLTO) build, flatpak bundle, checksums, GitHub release draft. |

### 15.2 Editor integration (clangd on a Chromium-sized tree)
- `compile_commands.json` from `just compdb`; `.clangd` checked in:
  ```yaml
  CompileFlags:
    CompilationDatabase: out/dev
  Index:
    Background: Skip   # flip to Build if the machine has ≥64 GB and you want whole-tree xref
  ```
- Since you're on LazyVim: point lspconfig at
  `clangd --compile-commands-dir=out/dev --header-insertion=never --pch-storage=memory -j=8 --limit-results=200`.
  With `Background: Skip`, open-file indexing stays snappy even on the full tree; the launcher (tiny) can keep background indexing on via its own `.clangd`.
- clangd can run inside the `dev` container against the mounted tree if you don't want an Ubuntu-matched clangd on the Arch host (LazyVim + `--remote`/container-attached nvim both work; simplest is nvim-in-container for tree hacking, host nvim for launcher/scripts).

### 15.3 Debugging & profiling arsenal
- **First line:** Chrome DevTools over the SSH tunnel — JS/DOM/network *and* the Performance/Media panels (frame drops, decoder in use — this is how P4 verifies VA-API is actually active).
- **Native:** gdbserver flow above; `rr` on the workstation for nasty input/lifecycle races (record under Xvfb; Zen 4 workstation supported, don't bother on the Deck).
- **Tracing:** Perfetto/`chrome://tracing` categories (`media`, `gpu`, `input`, `power`) captured via DevTools protocol remotely — preferred over `perf` on the Deck (no debugger/toolchain footprint on SteamOS needed).
- **Watchpoints on regressions:** `just smoke` screenshot diffs catch Leanback server-side UI shifts early (R1/R2 canary — run nightly in CI against production youtube.com/tv).

### 15.4 Repo hygiene
- `pre-commit` hooks: clang-format check (both styles), shellcheck, `gn format --dry-run`, patch-series applies cleanly against `DEPS.pin`.
- `DEPS.pin` is the single source of truth for the Cobalt commit — bumping it is a reviewed PR that must pass `just smoke` on both presets.

---

## 16. Open questions for you
1. Confirm A1: software decode acceptable for the M1 milestone, VA-API before public release?
2. Name (§12)?
3. Build infra: is your workstation the build host + self-hosted CI runner, or should the plan budget for a rented large runner?
4. Beta channel preference: GitHub releases only, or also a `beta` Flathub branch later?
5. Do you want me to prepare the P0 spike scripts (checkout, gn args, Docker-based builder) *before* you hand over SSH, so day one on the Deck is deploy-and-test rather than setup?
