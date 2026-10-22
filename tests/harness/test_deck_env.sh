#!/usr/bin/env bash
# L0 tests that scripts/lib.sh EXPORTS DECK_HOST/DECK_PORT: the Python tools (cert.py, deckctl.py,
# deck-ci.py) read them from os.environ, so an unexported variable means "no Deck".
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1

LIB="scripts/lib.sh"
[ -f "$LIB" ] || { echo "error: missing $LIB" >&2; exit 1; }

pass=0 fail=0
ok()   { pass=$((pass + 1)); echo "  ok   - $1"; }
bad()  { fail=$((fail + 1)); echo "  FAIL - $1"; }

# with_auth <user> <ip> <port> <snippet>: run snippet after sourcing lib.sh in a scratch repo with
# a synthetic .steamdeck_auth. lib.sh derives REPO_ROOT from its own path: $tmp/scripts/lib.sh.
with_auth() {
  local user="$1" ip="$2" port="$3" snippet="$4"
  local tmp
  tmp="$(mktemp -d)"
  mkdir -p "$tmp/scripts"
  cp "$LIB" "$tmp/scripts/lib.sh"
  {
    [ -n "$user" ] && printf 'STEAMDECK_USER=%s\n' "$user"
    [ -n "$ip" ] && printf 'STEAMDECK_IP=%s\n' "$ip"
    [ -n "$port" ] && printf 'STEAMDECK_PORT=%s\n' "$port"
  } > "$tmp/.steamdeck_auth"
  # Unset first, or an exported DECK_HOST in the caller's environment would make this always pass.
  ( unset DECK_HOST DECK_PORT; cd "$tmp" && bash -c ". ./scripts/lib.sh 2>/dev/null; $snippet" )
  rm -rf "$tmp"
}

echo "── DECK_HOST/DECK_PORT must survive into a child process ──"

# `env` is a real child: it sees exported variables only, exactly as python3 does.
got="$(with_auth deck 10.0.0.5 22 'env | grep "^DECK_HOST=" | cut -d= -f2-')"
[ "$got" = "deck@10.0.0.5" ] \
  && ok "DECK_HOST reaches a child process (got '$got')" \
  || bad "DECK_HOST did not reach a child process (got '${got:-<empty>}', want 'deck@10.0.0.5')"

got="$(with_auth deck 10.0.0.5 2222 'env | grep "^DECK_PORT=" | cut -d= -f2-')"
[ "$got" = "2222" ] \
  && ok "DECK_PORT reaches a child process (got '$got')" \
  || bad "DECK_PORT did not reach a child process (got '${got:-<empty>}', want '2222')"

# STEAMDECK_PORT is optional; children must still see a port.
got="$(with_auth deck 10.0.0.5 '' 'env | grep "^DECK_PORT=" | cut -d= -f2-')"
[ "$got" = "22" ] \
  && ok "DECK_PORT defaults to 22 in a child" \
  || bad "DECK_PORT default did not reach a child (got '${got:-<empty>}')"

echo "── the export must not invent a Deck that was never configured ──"

# With no auth file, DECK_HOST must be empty, not something like '@'.
got="$(with_auth '' '' '' 'env | grep "^DECK_HOST=" | cut -d= -f2-')"
[ -z "$got" ] \
  && ok "DECK_HOST is empty when no user/ip is configured" \
  || bad "DECK_HOST fabricated a host from nothing (got '$got')"

echo "── the exact line, so a future edit cannot quietly drop it ──"
if grep -qE '^export DECK_HOST DECK_PORT$' "$LIB"; then
  ok "lib.sh exports DECK_HOST and DECK_PORT"
else
  bad "lib.sh no longer has 'export DECK_HOST DECK_PORT' — children will not see the Deck"
fi

echo
echo "test_deck_env: ${pass} passed, ${fail} failed"
[ "$fail" -eq 0 ] || exit 1
