#!/usr/bin/env bash
# L0 tests for the one thing scripts/lib.sh must hand to its children: the Deck's address.
#
# Why this file exists. lib.sh *set* DECK_HOST and DECK_PORT without exporting them. A shell variable
# that is not exported is invisible to every child process, and everything that actually reaches the
# Deck from Python reads them out of os.environ:
#
#     scripts/cert.py --deck      ->  `just cert-deck` exited 3 "no DECK_HOST" against a live Deck
#     scripts/deckctl.py          ->  so did `just power` and `just soak`, before they ever sampled
#     scripts/deck-ci.py          ->  teardown silently no-opped, leaving the app running on the Deck
#
# deck-ci.sh had already been patched for this in its own shim. A fix applied per-caller is a fix the
# next caller will not get, so the export moved into lib.sh and these tests pin it there.
#
# Needs no Deck, no container, no Chromium. Run: tests/harness/test_deck_env.sh
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1

LIB="scripts/lib.sh"
[ -f "$LIB" ] || { echo "error: missing $LIB" >&2; exit 1; }

pass=0 fail=0
ok()   { pass=$((pass + 1)); echo "  ok   - $1"; }
bad()  { fail=$((fail + 1)); echo "  FAIL - $1"; }

# Run a snippet in a shell that has sourced lib.sh, in a scratch repo carrying a synthetic
# .steamdeck_auth, so the real one (and the real Deck) are never involved.
#
# lib.sh derives REPO_ROOT from its OWN path (`dirname $BASH_SOURCE/..`) and cd's there before
# reading .steamdeck_auth. So the copy has to sit at $tmp/scripts/lib.sh for $tmp to be the root.
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
  # Unset first: an exported DECK_HOST in the caller's own environment would make this pass no
  # matter what lib.sh does -- the very check that cannot fail.
  ( unset DECK_HOST DECK_PORT; cd "$tmp" && bash -c ". ./scripts/lib.sh 2>/dev/null; $snippet" )
  rm -rf "$tmp"
}

echo "── DECK_HOST/DECK_PORT must survive into a child process ──"

# The regression test. `env` is a real child: it sees exported variables and nothing else. This is
# precisely what python3 sees.
got="$(with_auth deck 10.0.0.5 22 'env | grep "^DECK_HOST=" | cut -d= -f2-')"
[ "$got" = "deck@10.0.0.5" ] \
  && ok "DECK_HOST reaches a child process (got '$got')" \
  || bad "DECK_HOST did not reach a child process (got '${got:-<empty>}', want 'deck@10.0.0.5')"

got="$(with_auth deck 10.0.0.5 2222 'env | grep "^DECK_PORT=" | cut -d= -f2-')"
[ "$got" = "2222" ] \
  && ok "DECK_PORT reaches a child process (got '$got')" \
  || bad "DECK_PORT did not reach a child process (got '${got:-<empty>}', want '2222')"

# The default. STEAMDECK_PORT is optional; children must still see a port.
got="$(with_auth deck 10.0.0.5 '' 'env | grep "^DECK_PORT=" | cut -d= -f2-')"
[ "$got" = "22" ] \
  && ok "DECK_PORT defaults to 22 in a child" \
  || bad "DECK_PORT default did not reach a child (got '${got:-<empty>}')"

echo "── the export must not invent a Deck that was never configured ──"

# With no auth file at all, DECK_HOST must be empty/unset rather than exported as something like
# '@'. A runner that believes in a Deck at '@' reports transport failures instead of "no Deck".
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
