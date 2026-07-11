---
scope: durable
verified: 2026-07-08
sources:
  - .internal/steamdeck-cobalt-youtube-plan.md
---

# Locked decisions

Carries across Cobalt versions. Reversing one of these is a project-level pivot, not a bump.

## Product / engine (doc §1)

| # | Decision | Value |
|---|----------|-------|
| D1 | Engine base | Chrobalt (Chromium-based Cobalt), **26+ only** — verified 2026-07-08 that ≤25 is classic Starboard Cobalt (own renderer, no Blink) and unusable here. Pinned 26.eap / M114. |
| D2 | Content scope | Free YouTube + best-effort Widevine L3 (desktop `libwidevinecdm.so`, user-fetched) |
| D3 | Runtime | Game Mode (gamescope) only; Desktop Mode untested/unsupported in v1 |
| A1 | HW decode | **Ships software decode.** VA-API was tried and engaged on-Deck but renders green-band corruption (reverted 2026-07-10); the "VA-API required before Flathub" assumption is now in question. Any HW-decode re-enable is gated on a pixel-clean check (`b3fcd58` / `milestones/m114.md`), never decoder-name. |
| A3 | Ad-blocking / UI mods | None in v1 (keeps Flathub acceptance + takedown risk low) |

## Identity

- **Name: Deckback.** App id **`io.github.properrr.deckback`** (effectively permanent for Flathub).
- Trademark rules (no "YouTube"/"YT" in id or name, no logo derivatives, required description) —
  canonical in `docs/legal.md`.

## Test hardware  ← locked 2026-07-09

- **We have an OLED (Sephiroth) unit only. There is no LCD (Van Gogh) unit and none is planned.**
  All on-hardware verification is therefore OLED-only, permanently, unless a unit is acquired.
- **Consequence: LCD is "best-effort, untested" for v1**, not "supported". Say so in README and
  SUPPORT.md. Do not print battery, thermal, or decode numbers as applying to the LCD.
- **Do not delete or special-case LCD code paths.** The APU arch, panel resolution, and Steam virtual
  gamepad are identical; the code stays model-agnostic. What changes is what we may *claim*.
- **The AV1 dispute can no longer be closed in-house** (see `hardware.md`). Therefore
  **`steer_av1_unsupported` stays default-on for both models, indefinitely.** Steering to VP9/H.264
  costs nothing today (VP9 hardware decode is confirmed), so the safe default is free. Never enable
  AV1 on the strength of an OLED-only measurement.
- **Recruit an LCD owner for the P9 beta** — that is the only realistic path to LCD coverage, and it
  is a scheduling dependency, not a nice-to-have.

## Toolchain (doc §13)

- **Two toolchains, never mixed.** In-tree (`patches/`): C++17, hermetic Clang + in-tree libc++.
  Out-of-tree (`launcher/`): C++23, dual-clean Clang ≥18 **and** GCC 14, `-Werror`.

## Build environment  ← added from research 2026-07-08

- **Container base: Debian 12 (bookworm), matching Cobalt trunk** — upstream builds all images on
  `marketplace.gcr.io/google/debian12` (SHA-pinned). Supersedes the plan's original Ubuntu 22.04
  choice. Rationale: hermetic toolchain makes the base a "tool caddy" (glibc required — no
  Alpine/musl/distroless); matching upstream's distro is the "CI YAML is the docs" bet.
  See `milestones/m114.md` for the exact mirrored package lists (those are milestone-scoped).

## Cobalt checkout strategy  ← added 2026-07-08

- **Shallow, single-revision fetch** (`gclient sync --no-history`), pinned by `DEPS.pin`. We do not
  track Cobalt's internal git history — it's tens of GB and irrelevant to us.
- **Not a git submodule.** `/cobalt/` is a gclient-managed tree (writes `third_party/`, `buildtools/`,
  `.gclient_entries` into itself; gitignored) and still needs `gclient sync` for its DEPS, so a
  submodule would nest a perpetually-dirty giant tree for no gain. `DEPS.pin` is the tracked pin.
- **Layout (Chrobalt):** its `DEPS` is Chromium-style (keyed under `src/`), so the gclient solution
  is named **`src`** and the engine tree lives at **`cobalt/src`**, not `cobalt/` directly. gclient
  runs from the gclient root (`--name src`) so the solution dir isn't a bind-mount point (avoids the
  `_bad_scm` move that skips hooks). Scripts abstract this as `COBALT_TREE`/`CTR_TREE`.
- Our patch series still works on a shallow root: our commits sit on top of the pinned revision, so
  `git format-patch $pin..HEAD` (scripts/patch.sh) has the history it needs.
