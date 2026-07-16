#!/usr/bin/env bash
# L0 tests for scripts/lib/portal_poll.sh — the pure decisions behind `just portal-poll`
# (scripts/portal-poll.sh): what --poll-timeout value is acceptable, the exact drop-in text systemd
# will accept, and reading the effective poll back out of a `systemctl show` line.
#
# The load-bearing ones: valid_poll_seconds() is the gate that stops a bad value producing a drop-in
# systemd silently refuses; portal_dropin_content() must emit the empty `ExecStart=` reset (without
# it systemd APPENDS a second ExecStart, which a Type=dbus unit rejects) and must never compound
# --poll-timeout onto a base that already has one.
#
# Needs no Deck, no systemd, no container. Run: tests/harness/test_portal_poll.sh
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1

# shellcheck source=scripts/lib/portal_poll.sh
. scripts/lib/portal_poll.sh

pass=0 fail=0
ok() { echo "ok   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1"; fail=$((fail + 1)); }
eq() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1: want '$2' got '$3'"; fi; }
# code <name> <want-rc> <cmd...>
code() {
  local n="$1" want="$2"
  shift 2
  "$@"
  local got=$?
  if [ "$got" = "$want" ]; then ok "$n"; else bad "$n: want rc $want got $got"; fi
}

# ---- valid_poll_seconds (the gate) --------------------------------------------------------------
code "valid: 60"              0 valid_poll_seconds 60
code "valid: 1 (min)"         0 valid_poll_seconds 1
code "valid: 86400 (max)"     0 valid_poll_seconds 86400
code "invalid: 0"             1 valid_poll_seconds 0
code "invalid: empty"         1 valid_poll_seconds ""
code "invalid: trailing unit" 1 valid_poll_seconds 60s
code "invalid: negative"      1 valid_poll_seconds -5
code "invalid: over max"      1 valid_poll_seconds 86401
code "invalid: float"         1 valid_poll_seconds 1.5
code "invalid: absurdly long" 1 valid_poll_seconds 9999999

# ---- strip_poll_timeout -------------------------------------------------------------------------
eq "strip: removes a trailing token" "/usr/lib/flatpak-portal" \
  "$(strip_poll_timeout '/usr/lib/flatpak-portal --poll-timeout=60')"
eq "strip: no token left untouched" "/usr/lib/flatpak-portal" \
  "$(strip_poll_timeout '/usr/lib/flatpak-portal')"
eq "strip: token mid-line, keeps the rest" "/usr/lib/flatpak-portal --replace" \
  "$(strip_poll_timeout '/usr/lib/flatpak-portal --poll-timeout=15 --replace')"

# ---- portal_dropin_content ----------------------------------------------------------------------
want="[Service]
ExecStart=
ExecStart=/usr/lib/flatpak-portal --poll-timeout=60"
eq "dropin: clears then re-sets ExecStart" "$want" \
  "$(portal_dropin_content '/usr/lib/flatpak-portal' 60)"
eq "dropin: strips a pre-existing timeout from the base" "$want" \
  "$(portal_dropin_content '/usr/lib/flatpak-portal --poll-timeout=999' 60)"

# ---- poll_seconds_from_execstart ----------------------------------------------------------------
eq "parse: reads the active timeout (show form)" "60" \
  "$(poll_seconds_from_execstart '{ path=/usr/lib/flatpak-portal ; argv[]=/usr/lib/flatpak-portal --poll-timeout=60 ; ignore_errors=no }')"
eq "parse: default when no timeout present" "default" \
  "$(poll_seconds_from_execstart '{ path=/usr/lib/flatpak-portal ; argv[]=/usr/lib/flatpak-portal ; ignore_errors=no }')"
eq "parse: bare argv line" "15" \
  "$(poll_seconds_from_execstart '/usr/lib/flatpak-portal --poll-timeout=15')"
eq "parse: empty in -> default" "default" "$(poll_seconds_from_execstart '')"

echo "portal_poll: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
