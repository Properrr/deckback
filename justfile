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

# REQUIRED once per clone: enable the repo's git hooks (pre-push runs `just preflight`).
hooks:
    git config core.hooksPath .githooks
    @echo "git hooks enabled — .githooks/pre-push runs 'just preflight' before every push."

# No Chromium tree, Deck or Docker needed. target: shell|launcher|gn|all.
# The pre-push / pre-release gate: the same script CI runs.
preflight target="all":
    ./scripts/preflight.sh "{{target}}"

# L0 tests for the harness itself (no Deck, container or Chromium tree). CI runs this on every push.
test-harness:
    ./tests/harness/run.sh

# Refuses hardware gates (vaapi/power/soak/…) with exit 6, never fakes a pass. Needs Docker.
# Containerized sim of the GPU-independent layers. suite: all|launcher|shortcut|portal|reconnect.
sim suite="all":
    ./scripts/sim/run.sh "{{suite}}"

# `--full` adds probe, power and soak; `--dry-run` to look. A run that adjudicates nothing exits 3.
# Unattended on-Deck run: deploy -> launch -> gate -> cert, then tear down.
deck-ci *args:
    ./scripts/deck-ci.sh "$@"

#   -m gate  = must pass · -m probe = discovery of still-unverified things · -k touch = the grab test
# L2 on-Deck suite over SSH + CDP tunnel. App must be running there (`just remote-run`).
test-deck *args:
    ./scripts/test-deck.sh "$@"

# ninja compdb for clangd, symlinked to repo root.
compdb:
    ./scripts/compdb.sh

# Xvfb headless boot + DevTools-protocol assertion + screenshot (the CI gate).
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

# Overwrite the Deck's install with a locally-built .flatpak, keeping user data (the YouTube login).
deck-install-dev bundle="io.github.properrr.deckback.flatpak":
    ./scripts/deck-flatpak.sh dev "{{bundle}}"

# (Re)point the Deck's install at the published repo for updates, keeping user data.
deck-use-repo:
    ./scripts/deck-flatpak.sh repo

# Report which channel (dev bundle / official repo) the Deck's install is on.
deck-channel:
    ./scripts/deck-flatpak.sh status

# Tail remote app log + relevant journalctl over SSH.
logs:
    ./scripts/logs.sh

# Open ssh -L 9222 tunnel, print chrome://inspect instructions.
debug:
    ./scripts/debug.sh

# `set [seconds]` (default 60) | `restore` | `status` (default). Set FIRST, then relaunch Deckback:
# a portal restart orphans a running app's update monitor.
# Shrink the Deck's Flatpak-portal update poll (~30 min) for fast self-update debugging, or restore it.
portal-poll *args:
    ./scripts/portal-poll.sh {{args}}

# Format: tree clang-format on patch-touched files, gn format on .gn, clang-format on launcher/.
fmt:
    ./scripts/fmt.sh

# Git-based quilt: export a new patch from cobalt/ HEAD into patches/.
patch-new name:
    ./scripts/patch.sh new "{{name}}"

# Re-export all patches from cobalt/ history and regenerate patches/series.
patch-refresh:
    ./scripts/patch.sh refresh

# Needs a video playing on the VA-API decoder; refuses to sample otherwise.
# Poll Deck battery draw during playback -> CSV, adjudicated against the P4 <=9 W gate.
power seconds="300":
    ./scripts/power.sh "{{seconds}}"

# Reports the post-resume EPP change (steamos#2383) without gating on it. Needs a video playing.
# n x suspend/resume over SSH: app alive AND playback position advanced after every resume.
soak n="25":
    ./scripts/soak.sh "{{n}}"

# Needs a human tapping the panel and the app running with disable_touch:false.
# Does gamescope touch mode 4 (passthrough) deliver real touch events to Blink, or only pointer emulation?
touch-probe *args:
    python3 scripts/touch-probe.py {{args}}

# Each engine preset x {router, router+hide_cursor}, real touch via CDP Input.dispatchTouchEvent. Skips
# gamescope/XI2, so only a FAILURE is conclusive. `just touch-bisect "deck release"` picks presets.
# Off-Deck check (in the build container) that touch still reaches the gesture router.
touch-bisect *args:
    ./scripts/touch-bisect.sh {{args}}

# player: the TVHTML5 player's rate/quality API (needs a video PLAYING) · pairing: is "Link with TV
# code" reachable under our UA (needs a human in Leanback Settings). A negative answer exits 0.
# Feature probes (player|pairing): classify a mechanism before building on it; exit 3 = could not observe.
feature-probe *args:
    python3 scripts/feature-probe.py {{args}}

# Fails only on a REGRESSION against tests/cert/expectations/. NOT YouTube certification (docs/legal.md).
# L3 conformance: self-hosted js_mse_eme on 127.0.0.1, headless in the container. No Deck needed.
cert suite="conformance-test" preset="dev" *args:
    ./scripts/cert.sh "{{suite}}" "{{preset}}" {{args}}

# Adds real GPU decode; `playbackperf-*` is recorded as a trend and never fails a build.
# Same suites against the Deck. The app must already be running there (just run / just remote-run).
cert-deck suite="conformance-test" *args:
    ./scripts/cert.sh --deck "{{suite}}" {{args}}

# Static gates on the manifest + shipped metadata. Needs only the pack image (no engine, no Deck).
flatpak-lint:
    ./scripts/flatpak-lint.sh

# pack image -> flatpak-builder -> local repo + .flatpak bundle. preset: deck|release.
flatpak preset="deck":
    ./scripts/flatpak.sh "{{preset}}"

# Install flatpak + steamos-add-to-steam + print controller-layout steps.
install:
    ./scripts/install.sh

# Needs a staged engine (run `just flatpak` first). See flathub/SUBMISSION.md.
# Assemble a Flathub-PR-ready manifest set (extra-data engine tarball + checksums + pinned commit).
flathub-prep tag:
    ./scripts/flathub-prep.sh "{{tag}}"

# Prepare a release branch: bump VERSION, roll CHANGELOG, add the AppStream <release>. See RELEASING.md.
release-prep version:
    ./scripts/release-prep.sh "{{version}}"

# deck.gn (+ThinLTO) build, flatpak bundle + engine tarball, checksums, GitHub release draft.
release tag:
    ./scripts/release.sh "{{tag}}"

# Continues the LIVE repo's history by default (needs network); DECKBACK_REPO_FRESH=1 for an
# offline/local test site. See scripts/publish-repo.sh for the environment knobs.
# Build the hostable ostree repo from the .flatpak bundle + stage the GitHub-Pages site locally.
publish-repo bundle="io.github.properrr.deckback.flatpak" site="flatpak/pages-site":
    ./scripts/publish-repo.sh "{{bundle}}" "{{site}}"

# Install the host-side audio-repair user service. Runs ON the Deck, not from the workstation.
audio-repair:
    ./scripts/install-audio-repair.sh

# Runs ON the Deck in Desktop Mode, not from the workstation. See docs/SUPPORT.md.
# Install the host-side idle-nudge user service (keeps the screen on while a video plays).
idle-nudge:
    ./scripts/install-idle-nudge.sh

#   just uninstall            # keep sign-in    ·    just uninstall --purge   # full wipe
# Remove Deckback + every host-side trace; keeps app data (the sign-in) unless --purge. Runs ON the Deck.
uninstall *args:
    ./scripts/uninstall.sh "$@"
