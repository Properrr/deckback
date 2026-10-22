#!/usr/bin/env bash
# Pure helpers (no side effects) for scripts/portal-poll.sh and its L0 test: the systemd drop-in that
# sets flatpak-portal's --poll-timeout on the Deck, and its parsing.

# The user service we override, its drop-in directory (relative to $HOME on the Deck), and the single
# drop-in file we own (`restore` removes only this file).
DECKBACK_PORTAL_SERVICE="flatpak-portal.service"
DECKBACK_PORTAL_DROPIN_DIR=".config/systemd/user/flatpak-portal.service.d"
DECKBACK_PORTAL_DROPIN_NAME="zz-deckback-fastpoll.conf"
# Fallback only when the unit's own ExecStart can't be read.
DECKBACK_PORTAL_DEFAULT_BIN="/usr/lib/flatpak-portal"
# Message only: the cadence flatpak-portal uses when no --poll-timeout is set.
DECKBACK_PORTAL_DEFAULT_POLL_HINT="~1800s (≈30 min, flatpak-portal built-in)"

# valid_poll_seconds <value> — true iff an integer in [1, 86400].
valid_poll_seconds() {
  case "$1" in
  '' | *[!0-9]*) return 1 ;;
  esac
  [ "${#1}" -le 6 ] && [ "$1" -ge 1 ] && [ "$1" -le 86400 ]
}

# strip_poll_timeout <cmd> — drop any --poll-timeout=... token(s) and normalise whitespace.
strip_poll_timeout() {
  printf '%s\n' "$1" |
    sed -E 's/[[:space:]]*--poll-timeout=[^[:space:]]*//g; s/[[:space:]]+/ /g; s/^[[:space:]]+//; s/[[:space:]]+$//'
}

# portal_dropin_content <base-cmd> <seconds> — the drop-in replacing ExecStart with the base command
# plus --poll-timeout. The empty `ExecStart=` resets the list (a Type=dbus unit rejects a second one).
portal_dropin_content() {
  local base seconds
  base="$(strip_poll_timeout "$1")"
  seconds="$2"
  printf '%s\n' \
    "[Service]" \
    "ExecStart=" \
    "ExecStart=${base} --poll-timeout=${seconds}"
}

# poll_seconds_from_execstart <ExecStart> — the active --poll-timeout seconds, or "default". Accepts
# `systemctl show -p ExecStart` output or a bare argv line.
poll_seconds_from_execstart() {
  local s
  s="$(printf '%s\n' "$1" | grep -oE -- '--poll-timeout=[0-9]+' | head -n1 | sed 's/.*=//')"
  if [ -n "$s" ]; then echo "$s"; else echo default; fi
}
