# Deckback command runner. Every recipe delegates to scripts/*.sh so CI can call scripts directly.
# `just --list` for the menu. Config comes from .env (copy .env.example).
set dotenv-load := true
set positional-arguments := true

_default:
    @just --list

# One-time: build image, gclient config + first sync at DEPS.pin. Prints disk/RAM warnings first.
bootstrap:
    ./scripts/bootstrap.sh

# gclient sync -r $(cat DEPS.pin) + re-apply patches/. Refuses to run on a dirty tree.
sync:
    ./scripts/sync.sh

# gn gen out/<preset> from args/<preset>.gn inside the container. preset: dev|deck|asan.
gen preset="dev":
    ./scripts/gen.sh "{{preset}}"

# autoninja -C out/<preset> cobalt, plus the launcher via CMake. Default preset: dev.
build preset="dev":
    ./scripts/build.sh "{{preset}}"

# Build + test just the launcher (fits any runner — no Chromium tree needed).
launcher *args:
    ./scripts/launcher.sh "$@"

# L0 tests for the harness itself (shell/awk logic + the tests/deck pure helpers).
# No Deck, no container, no Chromium tree. CI runs this on every push.
test-harness:
    ./tests/harness/run.sh

# Unattended on-Deck run: deploy -> launch -> gate -> cert, then tear down. `--full` adds probe,
# power and soak. A run that adjudicates nothing exits 3, never 0. `just deck-ci --dry-run` to look.
deck-ci *args:
    ./scripts/deck-ci.sh "$@"

# L2 on-Deck suite over SSH + CDP tunnel. App must be running there (`just remote-run`).
#   -m gate  = must pass · -m probe = discovery of still-unverified things · -k touch = the grab test
test-deck *args:
    ./scripts/test-deck.sh "$@"

# ninja compdb for clangd, symlinked to repo root.
compdb:
    ./scripts/compdb.sh

# Xvfb headless boot + DevTools-protocol assertion + screenshot (the CI gate).
# The preset was not a parameter until 2026-07-10, so `just smoke` could only ever look at out/dev.
# On a tree that has only out/deck it started nothing, waited for a DevTools endpoint nobody was
# serving, and reported TRANSPORT (4) — "retry me" — for a build that did not exist.
smoke preset="dev":
    ./scripts/smoke.sh "{{preset}}"

# Strip build + rsync to ~/cobalt-yt/ on the Deck (no --delete: preserves a user-fetched CDM).
deploy host=env_var_or_default("DECK_HOST", ""):
    ./scripts/deploy.sh "{{host}}"

# SSH-launch on the Deck with flags from config/app.json, tee to a remote log.
run:
    ./scripts/run.sh

# Remote Game Mode launch via the installed Flatpak, without Steam UI interaction.
remote-run:
    ./scripts/remote-run.sh

# Tail remote app log + relevant journalctl over SSH.
logs:
    ./scripts/logs.sh

# Open ssh -L 9222 tunnel, print chrome://inspect instructions.
debug:
    ./scripts/debug.sh

# Format: tree clang-format on patch-touched files, gn format on .gn, clang-format on launcher/.
fmt:
    ./scripts/fmt.sh

# Git-based quilt: export a new patch from cobalt/ HEAD into patches/.
patch-new name:
    ./scripts/patch.sh new "{{name}}"

# Re-export all patches from cobalt/ history and regenerate patches/series.
patch-refresh:
    ./scripts/patch.sh refresh

# Refuses to sample unless a video is really playing on the VA-API decoder: an average taken over a
# paused video passes 9 W effortlessly and measures nothing. Needs a video playing on the Deck.
# Poll Deck battery draw during playback -> CSV, adjudicated against the P4 <=9 W gate.
power seconds="300":
    ./scripts/power.sh "{{seconds}}"

# Reports the post-resume EPP change (steamos#2383) without gating on it -- root-only, unfixable.
# Still NOT checked: audio restored, screen did not dim. Needs a video playing on the Deck.
# n x suspend/resume over SSH: app alive AND playback position advanced after every resume.
soak n="25":
    ./scripts/soak.sh "{{n}}"

# L3 conformance. Serves the pinned js_mse_eme on 127.0.0.1 (secure context for EME + no phone-home),
# fails only on a REGRESSION against tests/cert/expectations/. NOT YouTube certification (docs/legal.md).
# Self-hosted js_mse_eme, headless in the container. No Deck needed -- MSE/codec is engine behaviour.
cert suite="conformance-test" preset="dev" *args:
    ./scripts/cert.sh "{{suite}}" "{{preset}}" {{args}}

# Adds real GPU decode. `playbackperf-*` is recorded as a TREND and never fails a build.
# Same suites against the Deck. The app must already be running there (just run / just remote-run).
cert-deck suite="conformance-test" *args:
    ./scripts/cert.sh --deck "{{suite}}" {{args}}

# Static gates on the manifest + shipped metadata. Needs the pack image; needs NO engine, no
# checkout, no Deck — so CI can run it on every push, which is the only reason a gate ever catches
# anything. `just flatpak` runs it first.
flatpak-lint:
    ./scripts/flatpak-lint.sh

# pack image -> flatpak-builder -> local repo + .flatpak bundle. preset: deck|release.
flatpak preset="deck":
    ./scripts/flatpak.sh "{{preset}}"

# Install flatpak + steamos-add-to-steam + print controller-layout steps.
install:
    ./scripts/install.sh

# Assemble a Flathub-PR-ready manifest set (extra-data engine tarball + checksums + pinned commit).
# Needs a staged engine (run `just flatpak` first). See flathub/SUBMISSION.md.
flathub-prep tag:
    ./scripts/flathub-prep.sh "{{tag}}"

# Prepare a release branch: bump VERSION, roll CHANGELOG, add the AppStream <release>. See RELEASING.md.
release-prep version:
    ./scripts/release-prep.sh "{{version}}"

# deck.gn (+ThinLTO) build, flatpak bundle + engine tarball, checksums, GitHub release draft.
release tag:
    ./scripts/release.sh "{{tag}}"

# Build the hostable ostree repo from the .flatpak bundle + stage the GitHub-Pages site locally.
publish-repo bundle="io.github.properrr.deckback.flatpak" site="flatpak/pages-site":
    ./scripts/publish-repo.sh "{{bundle}}" "{{site}}"

# Install the host-side audio-repair user service. Runs ON the Deck, not from the workstation.
audio-repair:
    ./scripts/install-audio-repair.sh

# Install the host-side idle-nudge user service (keeps the screen on / no auto-suspend while a video
# plays). Runs ON the Deck in Desktop Mode, not from the workstation. See docs/SUPPORT.md.
idle-nudge:
    ./scripts/install-idle-nudge.sh
