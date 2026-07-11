---
scope: milestone
milestone: mNNN
cobalt_pin: PLACEHOLDER
verified: unverified
status: open
sources: []
---

# Cobalt <mNNN> — milestone findings

> Copy this file to `<mNNN>.md` at the start of each Cobalt bump (see `../../MIGRATION.md`).
> Everything here is presumed STALE until re-verified against `cobalt_pin`.

## Metadata
- **Chromium milestone:** (e.g. M114)
- **`DEPS.pin` commit:** PLACEHOLDER
- **Pinned on:** YYYY-MM-DD
- **Status:** bring-up | stable | superseded-by-<mNNN>

## Spikes (Phase 0)
| Spike | Question | Status | Finding |
|---|---|---|---|
| S0.1 | Builds + runs on generic Linux? Which Chromium milestone? | open | |
| S0.2 | Working Leanback UA? Attestation beyond UA? | open | |
| S0.3 | Library-CDM path (`enable_library_cdms`/`enable_widevine`) intact? | open | |
| S0.4 | Ozone x11/Wayland under gamescope? | open | |
| S0.5 | Chromium media pipeline vs. Starboard `SbPlayer`? VA-API feasible? | open | |
| S0.6 | Blink Gamepad API sees evdev? Leanback reacts under console UA? | open | |

## GN args
Which args in `args/*.gn` still exist / were renamed / were removed at this milestone.

## Leanback UA (S0.2)
Working UA string + feature checklist (voice search / sign-in / settings / quality). Seeds
`config/app.json:user_agent`.

## Media & VA-API (S0.5)
Decode path (mojo/FFmpeg vs SbPlayer), VA-API flag names + gating, format-steering patch location.

## Widevine (S0.3)
Library-CDM path state, CDM component version + hash, re-enable effort.

## Docker image sync
Package lists mirrored from `cobalt/docker/{linux,unittest}` at this commit — diff vs. the previous
milestone and update `docker/Dockerfile`. Note the `BASE` digest
(`marketplace.gcr.io/google/debian12@sha256:…`).

## Patch rebase
Status of each `patches/*.patch` against `cobalt_pin` (applies clean / needed rework / dropped).

## Open issues
Anything surprising that a future bumper needs to know.
