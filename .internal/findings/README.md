# Deckback knowledge base

Where findings and insights are registered so they survive people, sessions, and — critically —
the **yearly Cobalt trunk bump**. The design doc (`.internal/steamdeck-cobalt-youtube-plan.md`) is
the *intent*; this tree is the *evolving truth* discovered while building.

## The one idea: durable vs. milestone-scoped

Cobalt trunk moves ~a milestone a year and Leanback changes server-side under us. Most pain at bump
time comes from not knowing **which knowledge is still true**. So every finding is one of:

- **`durable/`** — true regardless of Cobalt version: architecture, hardware facts (Deck APUs,
  AV1), strategy, locked decisions, trademark/legal posture. Bumps do **not** invalidate these.
- **`milestones/<mNNN>.md`** — true only for one pinned commit: GN arg names, the working Leanback
  UA, the media/VA-API decode path, whether the library-CDM path is stripped, patch-rebase status.
  On a bump these are **presumed stale until re-verified**.

That split *is* the migration design (see [`../MIGRATION.md`](../MIGRATION.md)): durable carries
forward for free; milestone gets re-derived from a template. If a "durable" fact turns out to be
version-specific, move it into a milestone file — that's the signal the taxonomy is working.

## Frontmatter (every file starts with this)

```yaml
---
scope: durable | milestone
milestone: m138            # milestone scope only — matches milestones/<id>.md
cobalt_pin: <short-sha>    # milestone scope only — the DEPS.pin it was verified against
verified: unverified | YYYY-MM-DD
status: open | confirmed | rejected     # for spike-like claims
sources: []                # URLs / repo paths backing the finding
---
```

## Layout

```
.internal/
  findings/
    README.md                 # this file
    durable/                  # survives Cobalt bumps
      architecture.md         # layer model + the CDP-driven launcher bridge
      build-slimming.md       # which Chromium features are OFF in args/common.gn, and which must stay ON
      dbus-reconnect.md       # session-bus reconnect for the self-updater
      decisions.md            # locked decisions (toolchains, no-LCD, AV1-off…)
      feature-landscape.md    # what is worth building for the Deck, ranked from measured levers
      hardware.md             # Deck APU / AV1 dispute / VA-API decode + the pixel gate
      harness.md              # F1–F16 automation-honesty log (checks that lied)
      input-ux.md             # controller/UX/text-entry research
      keep-awake.md           # keeping the Deck awake during playback (host-side helper)
      one-line-install.md     # one-line install for a new Deck (design)
      packaging-toolchain.md  # CURRENT packaging facts (25.08, device=input)
      packaging.md            # packaging architecture (toolchain: packaging-toolchain.md)
      page-scripts.md         # the registry of scripts the launcher injects into youtube.com/tv
      pip.md                  # picture-in-picture dead-end
      platform.md             # SteamOS/gamescope levers (EPP, QAM, zram)
      preflight-parity.md     # local pre-push and CI run the same gate
      preview.md              # focus/hover preview + probe
      self-update.md          # self-update via the Flatpak portal
      strategy.md             # risk posture
      test-sim.md             # containerized SteamOS/gamescope test sim
      touch-gestures.md       # opt-in touch gestures (touch_gestures)
      touch-lock.md           # why an EVIOCGRAB lock cannot work; disable_touch makes touch inert
    milestones/
      TEMPLATE.md             # copy to <mNNN>.md on each bump
      m138.md                 # active pin (cobalt-27)
  MIGRATION.md                # the yearly-bump playbook
  TEST-PLAN.md                # test tiers + the honest tested/untested matrix
```

A finding's `verified:` date and TEST-PLAN.md §2 must agree. If a finding claims a behavior the
matrix lists as unverified, one of them is lying — fix it before building on either.

## Rules

1. A finding is not "registered" until it's in a file here with frontmatter and a source.
2. `verified:` is a date only when someone actually reproduced it against `cobalt_pin`. Spikes start
   `unverified` / `status: open`.
3. When `DEPS.pin` changes, you MUST run `MIGRATION.md`. Do not silently carry a milestone file
   forward.
4. Keep durable files short and principled; keep milestone files concrete and disposable.
