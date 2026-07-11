---
scope: durable
verified: 2026-07-08
sources:
  - .internal/steamdeck-cobalt-youtube-plan.md
---

# Strategy & risk posture (durable)

Approaches that stay valid across Cobalt versions. The *implementation* of each is milestone-scoped.

## Media

- **Format steering is the highest-leverage battery fix.** Shape `MediaCapabilities`/`canPlayType`
  to report AV1 unsupported and VP9/H.264 supported (and `powerEfficient=true` if a working
  hw-decode path ever lands — see below) so YouTube's ABR picks hw-friendly streams. This is
  independent of whether hw decode is done yet.
- **We ship software decode.** VA-API hardware decode was tried and engaged on-Deck but **renders
  green-band corruption on every frame** — reverted 2026-07-10; any re-enable is gated on a
  pixel-clean result, never a decoder-name check. See `hardware.md` and `milestones/m114.md`
  §"VA-API decode is VISUALLY CORRUPT". Do **not** assert "no AV1 hardware on either Deck" as fact:
  it's disputed (libva on the OLED advertises `AV1Profile0` under VLD; we've never observed AV1
  decoding *through* `VaapiVideoDecoder`, and the LCD is untested), so AV1 steering stays default-on
  regardless — see `hardware.md`.

## Identity & resilience

- Console UA + remotely-overridable, signed `config/app.json` so Leanback breakage (R1/R2) is a
  config push, not a 1–3 h rebuild. `just smoke` run nightly against production youtube.com/tv is
  the drift canary.

## Widevine (best-effort by design)

- Free YouTube is the product. DRM is L3 software CDM, user-fetched at first run, never
  redistributed. L3 caps DRM'd resolution — documented, not a bug. See `docs/legal.md`.

## Distribution

- Flatpak on `org.freedesktop.Platform//25.08`, zypak-sandboxed (never `--no-sandbox`). Minimal
  permissions. Self-hosted runner for Chromium-scale builds; free runners for lint only. Current
  packaging facts: `packaging-toolchain.md`.

## Risk register

Lives in the design doc §8 (R1–R8). Durable risks worth re-reading before each phase: R1 (Google
gating), R2 (trunk churn — mitigated by pinning + thin patch series + monthly rebase), R3 (VA-API
backports), R7 (CI cost). When a risk *materializes* at a specific milestone, log the specifics in
that milestone file.
