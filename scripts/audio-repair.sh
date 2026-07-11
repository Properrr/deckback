#!/usr/bin/env bash
set -euo pipefail

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

while :; do
  while read -r input_id; do
    [[ -n "$input_id" ]] || continue
    pactl set-sink-input-mute "$input_id" 0 || true
  done < <(
    pactl list sink-inputs 2>/dev/null | awk '
      /^Sink Input #/ { id=$3; sub(/^#/, "", id); deck=0; muted=0 }
      /pipewire.access.portal.app_id = "io.github.properrr.deckback"/ { deck=1 }
      /Mute: yes/ { muted=1 }
      /^$/ { if (deck && muted) print id }
      END { if (deck && muted) print id }
    '
  )
  sleep 2
done
