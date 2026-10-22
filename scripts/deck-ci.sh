#!/usr/bin/env bash
# Unattended on-Deck run: deploy -> launch -> gate -> cert. Thin shim over scripts/deck-ci.py.
#
#   scripts/deck-ci.sh                 # the regression subset
#   scripts/deck-ci.sh --full          # + probe, power, soak
#   scripts/deck-ci.sh --dry-run       # print the plan; touch nothing
#
# Exit codes are AGGREGATED (scripts/lib/deckci.py): 2 (a gate says the product is wrong) outranks
# 4 (transport), and a run in which nothing adjudicated exits 3, never 0.
. "$(dirname "$0")/lib.sh"

# deck-ci.py (including its teardown) reads these from its environment.
export DECK_HOST DECK_PORT

exec python3 "$(dirname "$0")/deck-ci.py" "$@"
