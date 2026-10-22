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
# Exported: the Deck-facing Python tools (deckctl.py, cert.py --deck, deck-ci.py) read them from the
# environment.
export DECK_HOST DECK_PORT

CONTAINER_ENGINE="${CONTAINER_ENGINE:-docker}"
# COBALT_SRC is the gclient root (bind-mounted at /src); the engine tree is its `src` solution.
# COBALT_TREE is that tree on the host, CTR_TREE the same tree in the container.
COBALT_SRC="${COBALT_SRC:-./cobalt}"
COBALT_TREE="$COBALT_SRC/src"
CTR_TREE="/src/src"

# cobalt/build/gn.py platform and the runnable target.
COBALT_PLATFORM="${COBALT_PLATFORM:-chromium_linux-x64x11}"
COBALT_TARGET="${COBALT_TARGET:-content_shell}"

# Map our preset -> gn.py build_type (gold is reserved for scripts/release.sh).
preset_buildtype() {
  case "$1" in
  deck) echo qa ;;
  *) echo devel ;;
  esac
}

# ---- exit-code taxonomy -------------------------------------------------------------------------
#   0  success
#   1  generic failure / a tool we shelled out to crashed
#   2  ASSERTION failed — the product is wrong (the only code that means a regression)
#   3  ENVIRONMENT/precondition missing — no checkout, no Docker, no config
#   4  TRANSPORT — Deck unreachable, SSH died, CDP endpoint absent. Retryable
#   5  USAGE — the script was invoked wrong
readonly EX_OK=0 EX_FAIL=1 EX_ASSERT=2 EX_ENV=3 EX_TRANSPORT=4 EX_USAGE=5

# fail <exit-code> <message...>
fail() {
  local code="$1"
  shift
  echo "error: $*" >&2
  exit "$code"
}

die() { fail "$EX_FAIL" "$@"; }                # generic failure
die_assert() { fail "$EX_ASSERT" "$@"; }       # the product is wrong
die_env() { fail "$EX_ENV" "$@"; }             # our environment is wrong
die_transport() { fail "$EX_TRANSPORT" "$@"; } # the wire is wrong
die_usage() { fail "$EX_USAGE" "$@"; }
info() { echo ">> $*" >&2; }

# ---- pinned lint tools (preflight.sh, fmt.sh and CI all resolve them here) -----------------------
# clang-format comes from PyPI at exactly this version; the venv is keyed by it.
CLANG_FORMAT_VERSION=23.1.3
DECKBACK_PREFLIGHT_VENV="${DECKBACK_PREFLIGHT_VENV:-${TMPDIR:-/tmp}/deckback-lint-cf${CLANG_FORMAT_VERSION}}"
_pinned_bootstrapped=0
pinned_tool() { # pinned_tool <system-name, or "" for venv only> <venv-binary> <pip-spec...>
  local sys=""
  [ -n "$1" ] && sys="$(command -v "$1" 2>/dev/null || true)"
  if [ -n "$sys" ]; then
    echo "$sys"
    return
  fi
  if [ ! -x "$DECKBACK_PREFLIGHT_VENV/bin/$2" ]; then
    [ "$_pinned_bootstrapped" -eq 0 ] && info "bootstrapping pinned lint tools into $DECKBACK_PREFLIGHT_VENV …"
    _pinned_bootstrapped=1
    python3 -m venv "$DECKBACK_PREFLIGHT_VENV" >/dev/null 2>&1 &&
      "$DECKBACK_PREFLIGHT_VENV/bin/pip" install -q "${@:3}" >&2 ||
      die_env "could not bootstrap pinned lint tools — install $1 (and shellcheck), or fix python3-venv/pip"
  fi
  echo "$DECKBACK_PREFLIGHT_VENV/bin/$2"
}
clang_format_bin() { pinned_tool "" clang-format "clang-format==$CLANG_FORMAT_VERSION"; }

# Is the Deck plugged in? Argument: the concatenated AC/ADP `online` nodes.
#   ac_is_online "$(cat /sys/class/power_supply/{AC,ACAD,ADP}*/online)"
# On AC, `current_now` is the charge current, so a power sample there must be refused.
ac_is_online() {
  case "$(printf '%s\n' "$1" | tr -d '[:space:]')" in
  *1*) return 0 ;;
  *) return 1 ;;
  esac
}

# Is the panel dark? Arguments: the eDP connector's `dpms` and the backlight `brightness`.
# dpms Off or brightness 0 = dark; an unreadable brightness is not proof of darkness.
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

# Does this `flatpak info --show-permissions` output grant evdev access (`devices=` input or all)?
#   flatpak_grants_input "$(flatpak info --show-permissions io.github.properrr.deckback)"
flatpak_grants_input() {
  case "$(printf '%s\n' "$1" | sed -n 's/^devices=//p')" in
  *input*) return 0 ;;
  *all*) return 0 ;;
  *) return 1 ;;
  esac
}

# The pinned Cobalt commit from DEPS.pin; dies on the placeholder.
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

# All Deck access goes through these: SSH/rsync on DECK_PORT, with sshpass only when
# STEAMDECK_PASSWORD is set.
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

# Exits 0 (with a SKIP line) when no Deck is reachable; for recipes meaningless without hardware.
deck_or_skip() {
  if [ -z "${DECK_HOST:-}" ] || ! deck_ssh -o ConnectTimeout=5 -o BatchMode=yes "$DECK_HOST" true 2>/dev/null; then
    info "SKIP: no Deck reachable (DECK_HOST='${DECK_HOST:-unset}' port ${DECK_PORT})."
    exit "$EX_OK"
  fi
}

# Extra `compose run` options for rootless Docker, in the array COMPOSE_ROOTLESS (empty otherwise).
# Rootless Docker maps the host user to container root, so run as root, make tar/tarfile ignore
# unmappable uids, and route flatpak's bwrap around the nested netns it cannot create.
compose_rootless_args() {
  COMPOSE_ROOTLESS=()
  if [ "$CONTAINER_ENGINE" = docker ] \
      && docker info -f '{{.SecurityOptions}}' 2>/dev/null | grep -q rootless; then
    COMPOSE_ROOTLESS=(--user 0:0 -e TAR_OPTIONS=--no-same-owner -e PYTHONPATH=/work/docker/rootless
      -e FLATPAK_BWRAP=/work/docker/rootless/bwrap)
  fi
}

# Run a command inside the dev container with the repo + cobalt tree mounted.
in_container() {
  require_engine
  compose_rootless_args
  COBALT_COMMIT="$(deps_pin)" \
    "$CONTAINER_ENGINE" compose run --rm "${COMPOSE_ROOTLESS[@]}" dev "$@"
}
