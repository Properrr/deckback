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
milestone: m114            # milestone scope only — matches milestones/<id>.md
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
      decisions.md            # locked decisions (toolchains, no-LCD, AV1-off…)
      hardware.md             # Deck APU / AV1 dispute / VA-API-decode-is-corrupt
      harness.md              # F1–F16 automation-honesty log (checks that lied)
      input-ux.md             # controller/UX/text-entry research
      packaging-toolchain.md  # CURRENT packaging facts (25.08, device=input)
      packaging.md            # SUPERSEDED by packaging-toolchain.md (architecture only)
      pip.md                  # picture-in-picture dead-end
      platform.md             # SteamOS/gamescope levers (EPP, QAM, zram)
      preview.md              # focus/hover preview + probe
      strategy.md             # risk posture
      touch-lock.md           # the EVIOCGRAB lock is DEAD; disable_touch resolves it
    milestones/
      TEMPLATE.md             # copy to <mNNN>.md on each bump
      m114.md                 # active pin — Phase 0 spike answers live here
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
