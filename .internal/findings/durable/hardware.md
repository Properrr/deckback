---
scope: durable
verified: 2026-07-08
sources:
  - .internal/steamdeck-cobalt-youtube-plan.md
---

# Hardware facts (Steam Deck)

Physical properties of the target devices — independent of Cobalt version. These constrain the
media pipeline permanently.

| Aspect | LCD (Van Gogh) | OLED (Sephiroth) |
|---|---|---|
| APU | Zen2 4c/8t + RDNA2 8CU, 7nm | same arch, 6nm |
| Video decode (VCN) | H.264, HEVC, VP9 ✔ / AV1 ✘ ⚠**DISPUTED** | H.264, HEVC, VP9 ✔ / AV1 ⚠**measured PRESENT** |
| Display | 1280×800 60 Hz | 1280×800 90 Hz, HDR |
| Wi-Fi | RTL8822CE (Wi-Fi 5) | QCNFA765 (Wi-Fi 6E) |
| Mic | dual mic array | dual mic array |
| RAM | 16 GB shared | 16 GB shared |

## ★ We only have an OLED unit (locked 2026-07-09)

No LCD (Van Gogh) unit exists in the project and none is planned (`decisions.md`). Everything below
that says "test both units" is **unexecutable**. What that changes:

| Fact | Transfers to LCD? |
|---|---|
| APU arch, 1280×800 panel, 16 GB RAM, Steam virtual gamepad (`28DE:11FF`) | **Yes** — identical code path |
| VP9/H.264 hardware decode via `VaapiVideoDecoder` | Very likely (same VCN family), **unverified** |
| **AV1 decode** | **Unknowable in-house.** Dispute is now permanently open |
| Power / battery numbers (≤9 W target) | **No.** Different process node (7 nm vs 6 nm); LCD draws more. Never publish OLED numbers as LCD numbers |
| QAM refresh-rate guidance (90→60→45 Hz sweep) | **No.** The LCD panel is 60 Hz only — the sweep is meaningless there |
| Resume/reconnect timing (`resume_probe_*`, `resume_reload_after_ms`) | **No.** Different Wi-Fi chip (RTL8822CE vs QCNFA765); tuned on Wi-Fi 6E only |
| **Touchscreen identity (`2808:1015`)** | **Unknown — and this cuts the other way, see below** |

⚠ **The hardcoded touchscreen id may be the wrong one for the unit we actually have.**
`launcher/src/touch.cpp` pins Focaltech **FTS3528 / `2808:1015`**. That id is sourced from a *blog
post about an LCD Deck* ([crocidb](https://crocidb.com/post/investigating-touchscreen-issue-steam-deck/)),
cited in `input-ux.md` §3 — it was **never measured on our OLED unit**, and `TouchGuard` has never
run on hardware. So dropping LCD does not shrink the touch risk; it concentrates it. The
name/capability fallback in `is_touchscreen()` is documented as covering "firmware/name drift" but is
in truth the **cross-model compatibility path**, and it is untested. **First on-Deck touch task: dump
the real VID:PID/name of the OLED panel and fix the constant if it differs.**

## Load-bearing consequences

- ⚠ **"AV1 has no hardware decode on either unit" is DISPUTED as of 2026-07-08 (S0.5).** On the OLED
  (APU 0932), libva reports **`AV1Profile0` under `VAEntrypointVLD`** — a hardware decode entrypoint —
  and Chromium independently reports AV1 `powerEfficient=true` after a real driver query. Evidence and
  caveats: `../milestones/m114.md` §"Media stack (S0.5)". Still unproven: an AV1 stream observed
  decoding *through* `VaapiVideoDecoder`, and the **LCD** unit (untested).
  **2026-07-09 corroboration:** Van Gogh implements **VCN 3.0** — the same decode-block generation
  as Navi 2, which includes AV1 decode
  ([Phoronix](https://www.phoronix.com/news/AMD-Van-Gogh-AMDGPU-Linux)) — so the LCD likely has the
  hardware too. Dispute still open pending the VaapiVideoDecoder measurement (TASKS P4).
  **Until that is closed, keep steering YouTube to VP9/H.264** — VP9 hardware decode is confirmed
  working (`VaapiVideoDecoder`, 0 dropped frames), so steering costs us nothing today. Do not delete
  this row; resolve it with a measurement. This is a hardware fact, so it will not change with a
  Cobalt bump — but our belief about it was wrong once already.
  **2026-07-09 (locked):** with no LCD unit, the LCD half of this can never be measured here. The
  OLED half still can, and should be (force an AV1 stream, assert `kVideoDecoderName`). But
  **`steer_av1_unsupported` stays default-on regardless of how the OLED measurement lands** — an
  OLED-only result is not licence to enable AV1 for LCD users we cannot test. Steering is free.
- Render 1280×720 letterboxed to the 800p panel; cap 60 fps. HDR / 90 Hz are stretch goals.
- Resume-from-sleep network reconnection timing differs by Wi-Fi chip — **only the OLED's QCNFA765
  (Wi-Fi 6E) can be tuned here.** The LCD's RTL8822CE path is untuned and unmeasured; keep the resume
  backoff conservative rather than fitting it tightly to the one chip we can see.
- Power target (durable goal): 1080p VP9 ≤ ~8–9 W with hw decode (vs ~12–15 W sw). **This is an
  OLED-measured target.** The LCD's 7 nm APU draws more; do not restate the number for it.

## ★ Hardware VA-API decode renders CORRUPT through ANGLE — verify PIXELS, not just the decoder name

> **RESOLVED on M138 / cobalt-27 (2026-07-10, OLED).** The corruption below was **m114-specific**. On
> M138 the SAME two flags (`--enable-features=VaapiVideoDecodeLinuxGL --use-angle=gl`) were re-tested
> on-Deck through the shipped flatpak (Game Mode, real Leanback VP9 playback) under the mandatory DUAL
> gate: **decoder = `VaapiVideoDecoder` + `kIsPlatformVideoDecoder=true` AND a composited screenshot
> that passes `video_corruption_verdict` — 0.0% green-band rows across 5 sampled frames, visually
> confirmed clean.** M138's newer ANGLE/Chromium fixes the DMA-buf/EGLImage import on radeonsi. So on
> M138, VA-API hardware decode both engages AND displays correctly. **Power (2026-07-10, OLED, on
> battery): `just power` measured mean 5.91 W over 120 samples under `VaapiVideoDecoder` — PASS vs the
> ≤9 W gate (playback confirmed before AND after the window).** `config/app.json` now ships HW decode
> (flags in `cobalt_flags`). Only the LCD unit remains unknowable. **The durable
> lesson below stands regardless** — this result was only trustworthy *because* it was a pixel check;
> the decoder-name metrics would have passed on m114 too. The m114 evidence is kept as the historical
> record of the failure mode and why the pixel gate exists.

Measured on-Deck 2026-07-10 (OLED, Mesa/RADV, Cobalt m114): with VA-API hardware decode engaged
(`VaapiVideoDecoder`, `kIsPlatformVideoDecoder=true`), **every video frame shows green horizontal
banding** — ANGLE's DMA-buf/EGLImage import of the tiled NV12 VA surface is broken on this GPU stack
(tiled surface read as linear). A 7-variant flag bisection found the corruption **inseparable from
`VaapiVideoDecoder`**: every clean configuration had silently reverted to **software** `VpxVideoDecoder`.
Full evidence: `../milestones/m114.md` §"VA-API decode is VISUALLY CORRUPT". So `config/app.json`
ships **software decode** and P4 HW decode is **blocked** until the ANGLE↔VA-API import is fixed at
the engine/Mesa layer. Whether this recurs on the LCD (different, older Mesa/kernel) is unknowable
here; treat "VP9 hardware decode works on the Deck" as **decodes but does not display correctly**, not
as a shippable win.

**The durable lesson (survives any Cobalt/Mesa bump):** display-layer corruption is invisible to the
media pipeline. `kVideoDecoderName`, `kIsPlatformVideoDecoder`, `corruptedVideoFrames=0` and
`droppedVideoFrames=0` **all reported PASS while the panel was full of green stripes** —
`corruptedVideoFrames` counts *decoder*-reported corruption, and this corruption is in the *import* of
a correctly-decoded surface. This is an F1 miss ("a check that cannot fail is not a check"). **Any
hardware-decode gate MUST include a pixel/screenshot check** (e.g. reject frames dominated by
pure-green rows), never decoder identity alone. A green screen that passes every metric is exactly the
failure mode this project exists to catch.
