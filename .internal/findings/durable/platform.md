---
scope: durable
topic: steamos-gamescope-platform
verified: 2026-07-09
sources: deep-research pass 2026-07-09 (25 claims, 3-vote adversarial verify — partial, session-limited; load-bearing flag claims re-verified against the pinned M114 tree instead) — URLs inline
---

# SteamOS / gamescope platform facts & optimization levers

Platform-side (not Cobalt-version-side) facts driving the P4 power/smoothness work. Hard
constraint: everything must work from a Flatpak/user session on an immutable rootfs — root-only
levers are marked infeasible. M114-specific flag facts live in `../milestones/m114.md`
§"Platform-optimization research" (re-derive on bump).

## Compositor (gamescope, Game Mode)

- **Direct scanout is real and worth qualifying for** (verified 3-0): embedded gamescope can flip
  client frames straight to the display via DRM/KMS "even when stretching or when notifications
  are up", removing the composite copy. [gamescope README](https://github.com/ValveSoftware/gamescope)
  Our lever: keep the window exactly **1280×800 fullscreen**
  (`--content-shell-host-window-size=1280x800`; the page letterboxes the 720p video internally) so
  no gamescope scaling/filtering is ever needed. Verify on-Deck that our surface actually gets
  flipped (P4 task).
- When gamescope must composite, it uses async Vulkan compute for low-latency composition (same
  README, unverified quote) — the fallback costs a copy, not frame pacing.
- **Native Wayland clients need gamescope `--expose-wayland`** (unverified, ArchWiki), and Game
  Mode's gamescope is Valve-configured — so `--ozone-platform=x11` (Xwayland) stays the right
  choice (matches the S0.4 on-Deck result).
- Scaling filters (`-F fsr|nis|nearest`, `-S integer|stretch|fit`) only matter when client res ≠
  panel res — moot while we render native 800p.
- **The per-app perf knobs are user-facing QAM settings** (refresh rate 45–90 Hz on OLED,
  framerate cap, TDP limit), not gamescope flags we can pass — the claim that `-r`/`-o` gives a
  per-app cap was REFUTED (1-2). Consequence: battery guidance ships in SUPPORT.md; measurement
  happens in `just power`.

## Power

- **SteamOS resets EPP to `performance` on CPU cores 1–7 after suspend/resume** (verified 3-0 +
  2-0; observed on SteamOS 3.8.2 beta, only CPU0 keeps `balance_performance`), with measurably
  higher draw and heat: [valvesoftware/steamos#2383](https://github.com/valvesoftware/steamos/issues/2383).
  Deckback survives resume by design (P6), so it silently inherits the penalty every sleep cycle
  on affected builds. Writing EPP back needs root → **infeasible from the Flatpak**; we can only
  measure (post-resume delta in `just soak`) and document. Track the issue.
- OLED panel defaults to 90 Hz in Game Mode: for 60 fps-capped video, QAM refresh 60 (or 45) Hz
  cuts compositor + panel work — quantify in `just power`, recommend in SUPPORT.md.
- Wi-Fi power save toggling (`iw … set power_save`) is root-only → infeasible; the P6 resume probe
  backoff already covers the reconnect side.
- **Audio wakeups are tunable from the user session**: PipeWire quantum via the pulse-compat path
  Chromium uses (`PULSE_LATENCY_MSEC` env) and Chromium's `--audio-buffer-size=<frames>` (switch
  verified present in M114). Bigger buffers = fewer wakeups = better idle residency during
  playback; must verify A/V sync unharmed.

## Memory / storage

- SteamOS 3.6+ defaults to **zram** swap (~half of RAM, zstd, swappiness 60 — single unverified
  source): memory pressure on the 16 GB shared LPDDR5 is survivable, but decode surfaces and
  Chromium heaps compete with the GPU carve-out. Single-origin kiosk lever: `--process-per-site`
  (verified present in M114) collapses renderer count.
- Shader/Gr caches and the profile live under `--data-path` (persistent since the P1 flag fix) —
  keep the Flatpak on internal storage; a microSD-resident profile slows cold start (unmeasured).

## Quirks

- **Steam overlay (`gameoverlayrenderer.so`) LD_PRELOAD injection** is a documented source of
  gradual framerate degradation; `LD_PRELOAD=""` in the shortcut's launch options is the standard
  workaround (unverified, ArchWiki Steam/Troubleshooting). Keep in the troubleshooting drawer for
  Game-Mode-only hitches that SSH launches don't reproduce.
- **Van Gogh (LCD) implements VCN 3.0** — the decode-block generation that includes AV1 decode on
  Navi 2 ([Phoronix](https://www.phoronix.com/news/AMD-Van-Gogh-AMDGPU-Linux), unverified) —
  corroborates (does not resolve) the AV1 dispute in `durable/hardware.md`.
- **Zero-copy VA-API is settled: tolerate one copy.** Platform side, radeonsi exports decoded
  surfaces as disjoint dma-buf planes, breaking Chromium's zero-copy import on AMD; workarounds
  only apply under X11/XWayland with Vulkan output (unverified,
  [thubble gist](https://gist.github.com/thubble/235806c4c64b159653de879173d24d9f)). Engine side,
  M114 has no zero-copy-GL feature at all (tree-verified, m114.md). The plan §P4 "zero-copy if
  achievable (else tolerate one copy)" hedge is resolved to the fallback.
