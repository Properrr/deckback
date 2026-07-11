#!/usr/bin/env bash
# Unattended on-Deck run (T-phase): deploy -> launch -> gate -> cert, with no human in the loop.
#
#   scripts/deck-ci.sh                 # the regression subset
#   scripts/deck-ci.sh --full          # + probe, power, soak
#   scripts/deck-ci.sh --dry-run       # print the plan; touch nothing
#
# Exit codes are AGGREGATED, not concatenated (.internal/HARNESS.md §1). 2 (a gate says the product
# is wrong) outranks 4 (the wire broke), so a regression is never masked by an SSH drop in a later
# stage. And a run in which nothing adjudicated exits 3, never 0 — see scripts/lib/deckci.py.
#
# Everything here is a thin shim so CI can call the Python directly.
. "$(dirname "$0")/lib.sh"

# lib.sh SETS these; it does not export them. Without this line deck-ci.py's teardown reads
# DECK_HOST=None from its environment and quietly does nothing — leaving the app running on the Deck,
# where the next `just power` samples a video the previous run started and reports a number about the
# wrong thing. A cleanup step that silently no-ops is worse than no cleanup step, because you stop
# looking for the mess. tests/harness/test_deckci.py asserts this line still exists.
export DECK_HOST DECK_PORT

exec python3 "$(dirname "$0")/deck-ci.py" "$@"
