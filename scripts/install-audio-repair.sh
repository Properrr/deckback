#!/usr/bin/env bash
set -euo pipefail

install -Dm755 "$(dirname "$0")/audio-repair.sh" "$HOME/.local/bin/deckback-audio-repair"
install -Dm644 "$(dirname "$0")/deckback-audio-repair.service" \
  "$HOME/.config/systemd/user/deckback-audio-repair.service"
systemctl --user daemon-reload
systemctl --user enable deckback-audio-repair.service
systemctl --user restart deckback-audio-repair.service   # restart, so a reinstall picks up new code
systemctl --user --no-pager --full status deckback-audio-repair.service
