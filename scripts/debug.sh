#!/usr/bin/env bash
. "$(dirname "$0")/lib.sh"
require_deck
port=9222
info "Opening ssh -L ${port} tunnel to ${DECK_HOST}."
info "Then open chrome://inspect on this workstation and add localhost:${port} as a target,"
info "or attach the Performance/Media panels to verify decode path (P4)."
deck_ssh -N -L "${port}:localhost:${port}" "$DECK_HOST"
