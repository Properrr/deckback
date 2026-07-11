#!/usr/bin/env bash
. "$(dirname "$0")/lib.sh"
require_deck
# Game Mode render target: gamescope exposes Xwayland :0/:1 under XDG_RUNTIME_DIR (findings m114).
# --no-sandbox is bring-up only (no zypak until Phase 8) — passed via DECKBACK_EXTRA_ARGS so it never
# gets baked into the shipped config/app.json. The launcher boots content_shell on about:blank and
# injects the TV UA + navigates over CDP (content_shell ignores --user-agent).
DECK_DISPLAY="${DECK_DISPLAY:-:1}"
info "Launching Deckback on ${DECK_HOST} (DISPLAY=${DECK_DISPLAY}; Ctrl-C to detach; app keeps running) ..."
deck_ssh -t "$DECK_HOST" "
  cd ~/cobalt-yt &&
  XDG_RUNTIME_DIR=/run/user/\$(id -u) DISPLAY=${DECK_DISPLAY} \
  DECKBACK_COBALT_BIN=./content_shell \
  DECKBACK_EXTRA_ARGS='--no-sandbox' \
  ./deckback-launcher --config ./config/app.json 2>&1 | tee -a ~/cobalt-yt/deckback.log
"
