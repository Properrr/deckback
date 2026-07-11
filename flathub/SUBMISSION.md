# Submitting Deckback to Flathub

This directory holds everything needed to **attempt** a Flathub submission, plus an honest account of
the one thing that makes it hard. Read the "Reality check" first — it changes the plan.

## ⚠️ Reality check — the prebuilt-engine problem

Flathub's [requirements](https://docs.flathub.org/docs/for-app-authors/requirements) are explicit:

> All source available submissions must be built entirely from source code.
> Binary or precompiled files must not be present in the submission pull request.

Deckback's engine is **Cobalt** ("Chrobalt"), a Chromium fork. A from-source Chromium build needs
~45 GB of tree, tens of GB of RAM, and hours on a big machine — it **cannot** build inside Flathub's
CI. So the standard "build from source" route is not open to us. There are three ways to respond:

| Path | What it means | Verdict |
|---|---|---|
| **A · Build engine from source on Flathub** | Compile Cobalt in Flathub CI | ❌ Infeasible (resource/time limits) |
| **B · `extra-data` engine** | Flathub builds only the launcher/zypak from source; the engine is downloaded **at install time** from a Deckback GitHub release, pinned by `sha256`. `flathub/io.github.properrr.deckback.yaml` is written this way. | ⚠️ Possible, but reviewers may reject a self-built FOSS engine delivered as `extra-data` (that mechanism is meant for *non-redistributable* blobs). Needs a maintainer conversation up front. |
| **C · Self-hosted Flatpak repo** | Publish the ostree repo `just flatpak` already produces, plus a `.flatpakref`. Users add the remote once and then get **automatic background updates** — no Flathub review, no source-build rule. | ✅ Works today. **Recommended** near-term. |

**Recommendation:** ship via **Path C** now (it answers "how do users get updates?" immediately), and
open a Path B conversation with Flathub in parallel. Do not open a Flathub PR expecting a clean pass —
raise the `extra-data` engine question in a
[Matrix/Discourse](https://discourse.flathub.org/) thread first.

## What's in this directory

| File | Purpose |
|---|---|
| `io.github.properrr.deckback.yaml` | The Flathub submission manifest (Path B: `extra-data` engine, source-built launcher + zypak, licenses installed). Placeholder `sha256`/`size`/`commit` are filled by the prep script. |
| `flathub.json` | Flathub build config — `only-arches: [x86_64]` (the Deck is x86_64; there is no aarch64 build). |
| `apply_extra` | Runs at install time inside the sandbox to unpack the downloaded engine tarball into place. |

## Prerequisites (one-time)

1. **Tag a release** so the engine tarball and the source commit are immutable URLs:
   ```sh
   just release v0.0.1        # builds out/release, the .flatpak bundle, AND a
                              #   cobalt-prebuilt-<pin>.tar.zst engine tarball (see scripts/release.sh)
   ```
   Upload `cobalt-prebuilt-<pin>.tar.zst` as a GitHub **release asset**.
2. **Fill the manifest** with the real checksums + commit:
   ```sh
   just flathub-prep v0.0.1   # computes sha256 + size of the engine tarball, pins the source commit,
                              #   rewrites flathub/io.github.properrr.deckback.yaml, validates metainfo
   ```

## Submitting (per the official flow)

Flathub's [submission guide](https://docs.flathub.org/docs/for-app-authors/submission):

1. Fork `flathub/flathub` (uncheck "copy the master branch only").
2. ```sh
   git clone --branch=new-pr git@github.com:<you>/flathub.git
   cd flathub
   git checkout -b io.github.properrr.deckback new-pr
   cp <deckback>/flathub/io.github.properrr.deckback.yaml .
   cp <deckback>/flathub/flathub.json .
   cp <deckback>/flathub/apply_extra .
   git add . && git commit -m "Add io.github.properrr.deckback"
   git push origin io.github.properrr.deckback
   ```
3. Open a PR **against the `new-pr` base branch** (not `master`), titled `Add io.github.properrr.deckback`.
4. When a reviewer says it's ready, comment **`bot, build`** to trigger a test build.

## App-ID verification

`io.github.properrr.deckback` is under the `io.github.*` namespace, so ownership is proven by the
existence of <https://github.com/properrr/deckback>. If asked to verify, place the token at
`https://properrr.github.io/deckback/.well-known/org.flathub.VerifiedApps.txt` (needs GitHub Pages on
the repo).

## Pre-submission checklist

- [ ] `appstreamcli validate` passes on the metainfo (run `just flatpak-lint`).
- [ ] Screenshots resolve over HTTPS (they're on `raw.githubusercontent.com/.../main/...`, pushed).
- [ ] No `--no-sandbox`; zypak provides the sandbox (`just flatpak-lint` asserts this).
- [ ] `finish-args` justified in the PR description — expect questions on `--socket=x11` (Game Mode is
      gamescope Xwayland; no Wayland portal path), `--share=network` (it's a streaming client), and
      `--device=input` (evdev gamepad; no portal exists for it).
- [ ] Licenses installed to `/app/share/licenses/io.github.properrr.deckback/`.
- [ ] The Widevine CDM is **never** fetched in the Flathub build (see `docs/legal.md`). The manifest
      contains no CDM download; users add their own at runtime.
- [ ] `flatpak run org.flatpak.Builder --lint manifest flathub/io.github.properrr.deckback.yaml` clean.
