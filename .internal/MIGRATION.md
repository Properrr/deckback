# Cobalt bump playbook

Deckback tracks Cobalt trunk. Roughly yearly the pin moves to a new Chromium milestone; Leanback
and the build toolchain shift under us. This is the checklist that turns that from an archaeology
project into a routine. It is the operational half of the `durable/` vs `milestones/` split in
[`findings/README.md`](findings/README.md).

## Principle

**Durable knowledge carries forward for free; milestone knowledge is re-derived, not trusted.**
When `DEPS.pin` changes, every `findings/milestones/<old>.md` fact is presumed stale.

## Checklist — bumping `mOLD` → `mNEW`

1. **Branch.** `git switch -c bump/mNEW`.
2. **New milestone file.** `cp .internal/findings/milestones/TEMPLATE.md .internal/findings/milestones/mNEW.md`;
   fill metadata; mark `mOLD.md` status `superseded-by-mNEW`.
3. **Re-run the Phase 0 spikes** (S0.1–S0.6) against the new trunk. They are milestone-scoped for
   exactly this reason. Record answers in `mNEW.md`. Pay special attention to:
   - S0.1: exact Chromium milestone → set the real commit in `DEPS.pin`.
   - S0.3: did the library-CDM path get stripped/restored?
   - S0.5: did the media pipeline / VA-API flags change? **Known regression waiting past M115:**
     Chrome 116 broke Linux `VaapiVideoDecoder` (frame-pool init fails → silent FFmpeg software
     fallback; radeonsi affected on both ozone backends; still broken ≥123 —
     [crbug 40279587](https://issues.chromium.org/issues/40279587)). Re-prove
     `kVideoDecoderName=VaapiVideoDecoder` on-Deck before trusting any P4 power numbers.
4. **GN args.** Diff `args/*.gn` against the new trunk's expected args (renamed/removed?). CI job
   `gn-format` guards the `ffmpeg_branding` invariant; update presets as needed.
5. **Toolchain / image.** Re-diff `docker/Dockerfile`'s mirrored package lists against
   `cobalt/docker/{linux,unittest}` at the new commit, and update the `BASE`
   `marketplace.gcr.io/google/debian12@sha256:…` digest. Rebuild: `just bootstrap`.
6. **Rebase patches.** `just sync` re-applies `patches/series`. For each patch that fails:
   `just patch-refresh` after fixing in `cobalt/`. Record per-patch status in `mNEW.md`.
7. **UA / config.** Re-verify `config/app.json:user_agent` still yields full Leanback (S0.2). Push
   a config hotfix independently if only the UA moved (no rebuild needed).
8. **Green the gates.** `just launcher test` → `just build dev` → `just smoke` (both presets) →
   `just soak` + `just power` on both Deck units. `smoke` screenshot diff catches Leanback drift.
9. **Update pointers.** Bump the "active milestone" reference in `findings/README.md` layout note
   and any `.internal/TASKS.md` links. Update Claude memory (see below).
10. **Merge** once the QA matrix (doc §6 P9) is green on **OLED** (the only unit we have — see
    `findings/durable/decisions.md`). An LCD column is not a merge blocker because it can never be filled.

## What NOT to touch on a bump

`findings/durable/*` — architecture, hardware facts (AV1!), strategy, locked decisions,
trademark/legal. If a bump seems to invalidate one of these, that's a real finding: move it into
the milestone file and note why in the PR.

## Claude memory sync

The assistant's own memory has a milestone-scoped entry (`deckback-milestone.md`) and durable
entries. On a bump: rewrite/replace the milestone memory to point at `mNEW.md` and mark old
milestone facts obsolete. Durable memories stay. See `deckback-knowledge-structure` memory for the
protocol.
