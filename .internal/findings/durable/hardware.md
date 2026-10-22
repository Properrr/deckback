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
| Video decode (VCN) | H.264, HEVC, VP9 ✔ / AV1 ⚠ **disputed** | H.264, HEVC, VP9 ✔ / AV1 ⚠ **advertised by libva** |
| Display | 1280×800 60 Hz | 1280×800 90 Hz, HDR |
| Wi-Fi | RTL8822CE (Wi-Fi 5) | QCNFA765 (Wi-Fi 6E) |
| Mic | dual mic array | dual mic array |
| RAM | 16 GB shared | 16 GB shared |

## ★ We only have an OLED unit

No LCD (Van Gogh) unit exists in the project and none is planned (`decisions.md`). Every
on-hardware result is OLED-only. What transfers:

| Fact | Transfers to LCD? |
|---|---|
| APU arch, 1280×800 panel, 16 GB RAM, Steam virtual gamepad (`28DE:11FF`) | **Yes** — identical code path |
| VP9/H.264 hardware decode via `VaapiVideoDecoder` | Very likely (same VCN family), **unverified** |
| **AV1 decode** | **Unknowable in-house** — the dispute stays open |
| Power / battery numbers (≤9 W target) | **No.** Different process node (7 nm vs 6 nm); LCD draws more. Never publish OLED numbers as LCD numbers |
| QAM refresh-rate guidance (90→60→45 Hz sweep) | **No.** The LCD panel is 60 Hz only |
| Resume/reconnect timing (`resume_probe_*`, `resume_reload_after_ms`) | **No.** Different Wi-Fi chip; tuned on Wi-Fi 6E only |
| Touchscreen identity | Probably — the LCD id below comes from a third-party blog; never measured by us |

**Touchscreen:** Focaltech **FTS3528, `2808:1015`**, measured on the OLED as `FTS3528:00 2808:1015`
(`touch-lock.md`); the same id is reported for an LCD Deck by
[crocidb](https://crocidb.com/post/investigating-touchscreen-issue-steam-deck/). No shipping code
pins the id (the touch lock that did was deleted); it appears only in test fixtures.

## Measured on the OLED unit

- **libva decode profiles** (`VAEntrypointVLD`; radeonsi `vangogh`, Mesa 25.3, libva 1.22): MPEG2
  Simple/Main, VC1 Simple/Main/Advanced, H.264 ConstrainedBaseline/Main/High, HEVC Main/Main10, JPEG
  Baseline, VP9 Profile0/Profile2, **AV1 Profile0**. `vainfo` is not installed on SteamOS; query libva
  directly (e.g. Python `ctypes` against `libva.so`; no script is kept in the repo).
- **Steam Input's virtual pad.** In Game Mode the readable device is a virtual `Microsoft X-Box 360
  pad`; only it advertises `BTN_SOUTH`, the raw `Valve Software Steam Deck Controller` nodes do not.
  A newly attached or uinput-created pad makes Steam Input add a second Xbox-360 wrapper node; the
  launcher opens both and merges them without duplicate keys.

## Load-bearing consequences

- ⚠ **AV1 hardware decode is disputed, not absent.** On the OLED (APU 0932) libva advertises
  `AV1Profile0` under `VAEntrypointVLD`, and Chromium reports AV1 `powerEfficient=true` after a real
  driver query. Van Gogh implements **VCN 3.0**, the Navi 2 decode generation that includes AV1
  ([Phoronix](https://www.phoronix.com/news/AMD-Van-Gogh-AMDGPU-Linux)), so the LCD likely has it
  too. Never observed: an AV1 stream decoding *through* `VaapiVideoDecoder` (the OLED half is still
  measurable: force AV1, assert `kVideoDecoderName`). The LCD half can never be measured here.
  **`steer_av1_unsupported` stays default-on regardless of any OLED result** — VP9 hardware decode
  works, so steering costs nothing, and an OLED-only result is no licence to enable AV1 for LCD users.
- Render 1280×720 letterboxed to the 800p panel; cap 60 fps. HDR / 90 Hz are stretch goals.
- Resume-from-sleep reconnection timing differs by Wi-Fi chip — only the OLED's QCNFA765 can be
  tuned here. Keep the resume backoff conservative rather than fitted to one chip.
- Power target (durable goal): 1080p VP9 ≤ ~8–9 W with hardware decode (vs ~12–15 W software). An
  OLED target; do not restate it for the LCD.

## ★ Hardware VA-API decode renders CORRUPT through ANGLE — verify PIXELS, not just the decoder name

**Current state (M138, OLED):** `config/app.json` ships hardware decode. VA-API needs **both**
`--enable-features=VaapiVideoDecodeLinuxGL` and `--use-angle=gl` in `cobalt_flags`; with only the
feature flag ANGLE stays on Vulkan and decode silently falls back to software.
`--ignore-gpu-blocklist` / `VaapiIgnoreDriverChecks` are not needed and only suppress real checks.

- **Dual gate passed** on-Deck in Game Mode through the flatpak, real Leanback VP9: decoder
  `VaapiVideoDecoder` + `kIsPlatformVideoDecoder=true`, **and** a composited screenshot through
  `tests/deck/lib/probes.py:video_corruption_verdict` — 0.0% green-band rows across 5 frames,
  visually clean (2026-07-10). The L2 gates `test_hardware_decode_is_vaapi` and
  `test_video_is_not_corrupt` passed together on 2026-10-21.
- **Power:** `just power 120` on battery, mean **5.91 W over 120 samples** (V×I) under
  `VaapiVideoDecoder`, PASS vs ≤9 W, playback confirmed before and after (2026-07-10,
  `milestones/m138.md`). A 2026-10-21 run read 6.39 W (PASS) on SteamOS 3.8.28 with a different
  video and brightness — not comparable to that baseline.
- LCD: untested.

**The rule (survives any Cobalt/Mesa bump):** display-layer corruption is invisible to the media
pipeline. `kVideoDecoderName`, `kIsPlatformVideoDecoder`, `corruptedVideoFrames=0` and
`droppedVideoFrames=0` all report PASS while the panel shows green horizontal bands, because
`corruptedVideoFrames` counts *decoder* corruption and this corruption is in ANGLE's DMA-buf/EGLImage
import of a correctly-decoded tiled NV12 surface. The m114 engine did exactly that on this GPU stack.
**Any hardware-decode gate MUST include the composited pixel check** — never decoder identity alone
(a check that cannot fail, `harness.md` F1). After every Cobalt bump, re-run it before trusting the
two flags, and revert to software decode if it fails.
