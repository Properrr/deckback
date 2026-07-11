#!/usr/bin/env bash
# L2: run the on-Deck automated suite from the workstation (TEST-PLAN §3).
#
# The Deck is a dumb target: nothing is installed there, and everything is driven over SSH + a CDP
# tunnel. Runs on the WORKSTATION, never on the Deck.
#
# Exit codes (.internal/HARNESS.md): 0 pass · 2 a test failed (the product is wrong) ·
#                                    3 no Deck / no pytest (environment) · 5 usage.
#
# A SKIPPED suite is not a passing suite. If every test skips because the Deck is unreachable, this
# exits 3, not 0 — the same distinction `just power` once got wrong, in the same direction.
#
# Usage:
#   scripts/test-deck.sh                    # gates + probes
#   scripts/test-deck.sh -m gate            # only what must pass
#   scripts/test-deck.sh -m probe           # only the discovery tests (unverified things)
#   scripts/test-deck.sh -k touch           # one file / one test, by name
#   scripts/test-deck.sh --no-skip          # a missing Deck is an ERROR (for the unattended runner)
. "$(dirname "$0")/lib.sh"

command -v python3 >/dev/null 2>&1 || die_env "python3 not found on this workstation"
python3 -c 'import pytest' 2>/dev/null || die_env \
  "pytest not installed. Install it on the workstation (never on the Deck): pip install pytest"

# Fails with exit 3 (no host configured) or 4 (configured but unreachable) — the distinction the
# unattended runner needs in order to decide whether to retry.
require_deck

info "test-deck: target ${DECK_HOST} (port ${DECK_PORT})"

# `-p no:cacheprovider`: the repo is bind-mounted into the build container, and a root-owned
# .pytest_cache would break later container runs.
set +e
DECK_HOST="$DECK_HOST" DECK_PORT="$DECK_PORT" \
  python3 -m pytest tests/deck \
    -p no:cacheprovider \
    -o cache_dir=/tmp/deckback-pytest \
    --tb=short \
    -rs \
    "$@"
rc=$?
set -e

# pytest's exit codes: 0 ok · 1 tests failed · 2 interrupted · 3 internal error · 4 usage · 5 no tests
case "$rc" in
  0)
    info "test-deck: PASS"
    exit "$EX_OK"
    ;;
  1) die_assert "test-deck: a test failed — the product is wrong (see the report above)" ;;
  2) fail "test-deck: interrupted"; exit "$EX_FAIL" ;;
  5) die_env "test-deck: pytest collected no tests" ;;
  *) fail "test-deck: pytest internal/usage error (rc=$rc)"; exit "$EX_FAIL" ;;
esac
