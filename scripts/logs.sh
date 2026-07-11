#!/usr/bin/env bash
. "$(dirname "$0")/lib.sh"
require_deck
info "Tailing ~/cobalt-yt/deckback.log + journald on ${DECK_HOST} ..."
deck_ssh "$DECK_HOST" 'tail -F ~/cobalt-yt/deckback.log & journalctl -f -g deckback 2>/dev/null; wait'
