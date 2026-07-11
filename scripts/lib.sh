#!/usr/bin/env bash
# Common helpers sourced by every scripts/*.sh. Not executed directly.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# shellcheck disable=SC1091
[ -f .env ] && . ./.env
# Steam Deck SSH credentials (gitignored). Authoritative for the Deck target when present.
# shellcheck disable=SC1091
[ -f .steamdeck_auth ] && . ./.steamdeck_auth
DECK_PORT="${STEAMDECK_PORT:-22}"
if [ -n "${STEAMDECK_USER:-}" ] && [ -n "${STEAMDECK_IP:-}" ]; then
  DECK_HOST="${STEAMDECK_USER}@${STEAMDECK_IP}"
fi
# EXPORTED, not merely set. Every Deck-facing Python tool -- deckctl.py (so power.sh and soak.sh),
# cert.py --deck, deck-ci.py -- reads these from os.environ. A plain assignment is visible to this
# shell and invisible to its children, so those tools saw no Deck at all and exited 3 ("environment"):
# `just cert-deck` said `no DECK_HOST` against a Deck answering SSH on the next line. deck-ci.sh was
# patched for exactly this once, in its own shim; a fix that has to be repeated per caller is a fix
# that will be forgotten by the next caller. Pinned by tests/harness/test_deck_env.sh.
export DECK_HOST DECK_PORT

CONTAINER_ENGINE="${CONTAINER_ENGINE:-docker}"
# COBALT_SRC is the gclient ROOT (holds .gclient + the `src/` solution), bind-mounted at /src.
# Chrobalt (Chromium-based Cobalt) checks out with a solution named `src`, so the actual engine
# tree lives one level down. COBALT_TREE is that tree on the host; CTR_TREE is the same in-container.
COBALT_SRC="${COBALT_SRC:-./cobalt}"
COBALT_TREE="$COBALT_SRC/src"
CTR_TREE="/src/src"

# Cobalt 26 build system (findings/milestones/m114.md): builds go through cobalt/build/gn.py, which
# selects a named platform + build_type. Chrobalt on desktop Linux is `chromium_linux-x64x11`, and
# the runnable target is `content_shell` (Cobalt's code compiles into it). v1 is Linux-only.
COBALT_PLATFORM="${COBALT_PLATFORM:-chromium_linux-x64x11}"
COBALT_TARGET="${COBALT_TARGET:-content_shell}"

# Map our preset -> gn.py build_type. deck is the deployable (optimized, keeps diagnostics); dev/asan
# iterate. gold is reserved for tagged releases (scripts/release.sh).
preset_buildtype() {
  case "$1" in
  deck) echo qa ;;
  *) echo devel ;;
  esac
}

# ---- exit-code taxonomy -------------------------------------------------------------------------
# "The build failed" is useless at 3 a.m. Every script distinguishes *why* it failed, so an
# unattended runner (and an LLM reading only the exit code) can tell a broken environment from a real
# regression. TEST-PLAN §6.5 makes this mandatory for the cert harness; it is cheap everywhere else.
#
#   0  success
#   1  generic failure / a tool we shelled out to crashed
#   2  ASSERTION failed — the thing under test is genuinely wrong. This is the only code that means
#      "the product is broken". (scripts/cdp.py already exits 2 for a failed DOM assertion.)
#   3  ENVIRONMENT/precondition missing — no checkout, no Docker, no config. Not a product defect.
#   4  TRANSPORT — Deck unreachable, SSH died, CDP endpoint absent. Retryable; never a regression.
#   5  USAGE — the script was invoked wrong.
readonly EX_OK=0 EX_FAIL=1 EX_ASSERT=2 EX_ENV=3 EX_TRANSPORT=4 EX_USAGE=5

# fail <exit-code> <message...>
fail() {
  local code="$1"
  shift
  echo "error: $*" >&2
  exit "$code"
}

die() { fail "$EX_FAIL" "$@"; }                # generic failure (legacy callers)
die_assert() { fail "$EX_ASSERT" "$@"; }       # the product is wrong
die_env() { fail "$EX_ENV" "$@"; }             # our environment is wrong
die_transport() { fail "$EX_TRANSPORT" "$@"; } # the wire is wrong
die_usage() { fail "$EX_USAGE" "$@"; }
info() { echo ">> $*" >&2; }

# Does this `flatpak info --show-permissions` output grant evdev access?
#
#   flatpak_grants_input "$(flatpak info --show-permissions io.github.properrr.deckback)"
#
# `all` counts: --device=all is a superset of --device=input, and a user who granted it has a working
# gamepad. Our own manifest must never ask for it (scripts/flatpak-lint.sh forbids that), but
# refusing to *recognise* it would make `just install` fail for someone whose app works fine.
#
# Only the `devices=` line is consulted. `filesystems=/mnt/input` must not be mistaken for a device.
# Is the Deck plugged in? Argument is the concatenated contents of every AC/ADP `online` node.
#
#   ac_is_online "$(cat /sys/class/power_supply/{AC,ACAD,ADP}*/online)"
#
# A power measurement taken on AC is not a smaller version of the right answer, it is an answer to a
# different question: `current_now` becomes the CHARGE current, so P = V x I describes energy going
# INTO the battery. On this Deck that is ~4.6 W -- comfortably under the 9 W gate, entirely plausible,
# and about nothing. Refusing is the only safe reading. (F1's lesson, one layer out.)
ac_is_online() {
  case "$(printf '%s\n' "$1" | tr -d '[:space:]')" in
  *1*) return 0 ;;
  *) return 1 ;;
  esac
}

# Is the panel dark? Arguments: the eDP connector's `dpms`, and the backlight `brightness`.
#
# A draw sampled with the screen off understates a real session by watts, on a device whose whole
# gate is "<= ~9 W". The OLED blank/burn-in scripts people run over SSH turn the panel off AND, if
# they chvt away, deactivate the seat session -- so this doubles as a check that we are measuring the
# machine a user would actually be holding.
panel_is_dark() {
  local dpms="$1" brightness="$2"
  case "$dpms" in
  [Oo]ff) return 0 ;;
  esac
  case "$brightness" in
  '' | *[!0-9]*) return 1 ;; # unreadable: not proof of darkness, let the caller decide
  0) return 0 ;;
  esac
  return 1
}

flatpak_grants_input() {
  case "$(printf '%s\n' "$1" | sed -n 's/^devices=//p')" in
  *input*) return 0 ;;
  *all*) return 0 ;;
  *) return 1 ;;
  esac
}

# The pinned Cobalt commit. Guards against the placeholder shipped before spike S0.1.
deps_pin() {
  local pin
  pin="$(tr -d '[:space:]' <DEPS.pin)"
  case "$pin" in
  "" | PLACEHOLDER_SET_IN_S0.1)
    die "DEPS.pin is a placeholder. Set the real Cobalt commit (spike S0.1) before building."
    ;;
  esac
  echo "$pin"
}

require_engine() {
  command -v "$CONTAINER_ENGINE" >/dev/null 2>&1 ||
    die_env "container engine '$CONTAINER_ENGINE' not found (set CONTAINER_ENGINE in .env)"
}

require_cobalt_checkout() {
  [ -d "$COBALT_TREE/.git" ] ||
    die_env "no Cobalt checkout at '$COBALT_TREE'. Run 'just bootstrap' first."
}

# SSH/rsync to the Deck honoring the configured port and, only if provided, a password (via sshpass;
# key auth is the norm and needs neither).
deck_ssh() {
  if [ -n "${STEAMDECK_PASSWORD:-}" ] && command -v sshpass >/dev/null 2>&1; then
    sshpass -p "$STEAMDECK_PASSWORD" ssh -p "$DECK_PORT" "$@"
  else
    ssh -p "$DECK_PORT" "$@"
  fi
}
deck_rsync() {
  if [ -n "${STEAMDECK_PASSWORD:-}" ] && command -v sshpass >/dev/null 2>&1; then
    sshpass -p "$STEAMDECK_PASSWORD" rsync -e "ssh -p $DECK_PORT" "$@"
  else
    rsync -e "ssh -p $DECK_PORT" "$@"
  fi
}

require_deck() {
  [ -n "${DECK_HOST:-}" ] ||
    die_env "No Deck host. Set STEAMDECK_USER/STEAMDECK_IP in .steamdeck_auth (or DECK_HOST in .env)."
  deck_ssh -o ConnectTimeout=5 -o BatchMode=yes "$DECK_HOST" true 2>/dev/null ||
    die_transport "cannot SSH to '$DECK_HOST' port $DECK_PORT (Developer Mode + sshd + your key on the Deck?)"
}

# Skip cleanly (exit 0) when no Deck is attached, instead of failing a CI run that never had one.
# Used by recipes that are meaningless without hardware. Prints why, so a silent skip is impossible.
deck_or_skip() {
  if [ -z "${DECK_HOST:-}" ] || ! deck_ssh -o ConnectTimeout=5 -o BatchMode=yes "$DECK_HOST" true 2>/dev/null; then
    info "SKIP: no Deck reachable (DECK_HOST='${DECK_HOST:-unset}' port ${DECK_PORT})."
    exit "$EX_OK"
  fi
}

# Run a command inside the dev container with the repo + cobalt tree mounted.
in_container() {
  require_engine
  COBALT_COMMIT="$(deps_pin)" \
    "$CONTAINER_ENGINE" compose run --rm dev "$@"
}
