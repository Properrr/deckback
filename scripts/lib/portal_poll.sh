#!/usr/bin/env bash
# Pure helpers for the Flatpak-portal poll override (scripts/portal-poll.sh). No SSH, no systemd, no
# side effects — so the drop-in content and the poll-interval parsing that `just portal-poll` relies
# on are unit-testable at L0 (tests/harness/test_portal_poll.sh). Sourced by both the script and its
# test.
#
# Background (findings/durable/self-update.md): flatpak-portal's UpdateMonitor only re-checks the
# remote on its own cadence — a ~30 min built-in default — so a self-update round-trip normally waits
# half an hour before the first detection. `--poll-timeout=<seconds>` shrinks that; this drop-in is
# the reversible way to set it on the Deck.

# The user service we override, its drop-in directory (relative to $HOME on the Deck), and the single
# drop-in file we own. `restore` removes exactly this file, so unrelated drop-ins are never touched.
DECKBACK_PORTAL_SERVICE="flatpak-portal.service"
DECKBACK_PORTAL_DROPIN_DIR=".config/systemd/user/flatpak-portal.service.d"
DECKBACK_PORTAL_DROPIN_NAME="zz-deckback-fastpoll.conf"
# Where flatpak-portal usually lives; only a fallback when the unit's own ExecStart can't be read.
DECKBACK_PORTAL_DEFAULT_BIN="/usr/lib/flatpak-portal"
# flatpak-portal sets no --poll-timeout by default; its built-in cadence is ~30 min. Message only.
DECKBACK_PORTAL_DEFAULT_POLL_HINT="~1800s (≈30 min, flatpak-portal built-in)"

# valid_poll_seconds <value> — true iff a positive integer in [1, 86400] (1 day). Rejects empty,
# non-numeric, negative, float, and out-of-range so `set` never writes a drop-in systemd will refuse.
valid_poll_seconds() {
  case "$1" in
  '' | *[!0-9]*) return 1 ;;
  esac
  [ "${#1}" -le 6 ] && [ "$1" -ge 1 ] && [ "$1" -le 86400 ]
}

# strip_poll_timeout <cmd> — drop any existing --poll-timeout=... token(s) from a command string and
# normalise whitespace. Guards against compounding the flag when the base command already carries one.
strip_poll_timeout() {
  printf '%s\n' "$1" |
    sed -E 's/[[:space:]]*--poll-timeout=[^[:space:]]*//g; s/[[:space:]]+/ /g; s/^[[:space:]]+//; s/[[:space:]]+$//'
}

# portal_dropin_content <base-cmd> <seconds> — the systemd drop-in that replaces ExecStart with the
# base command plus --poll-timeout. The empty `ExecStart=` is required: systemd appends to a unit's
# ExecStart list otherwise, and a service of Type=dbus rejects a second ExecStart.
portal_dropin_content() {
  local base seconds
  base="$(strip_poll_timeout "$1")"
  seconds="$2"
  printf '%s\n' \
    "[Service]" \
    "ExecStart=" \
    "ExecStart=${base} --poll-timeout=${seconds}"
}

# poll_seconds_from_execstart <systemctl-show-ExecStart> — echo the active --poll-timeout seconds, or
# "default" when none is set. Accepts both `systemctl show -p ExecStart` output and a bare argv line.
poll_seconds_from_execstart() {
  local s
  s="$(printf '%s\n' "$1" | grep -oE -- '--poll-timeout=[0-9]+' | head -n1 | sed 's/.*=//')"
  if [ -n "$s" ]; then echo "$s"; else echo default; fi
}
