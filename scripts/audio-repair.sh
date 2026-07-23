#!/usr/bin/env bash
set -euo pipefail

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

APP="io.github.properrr.deckback"
BIN="$HOME/.local/bin/deckback-audio-repair"
UNIT="deckback-audio-repair.service"
UNIT_PATH="$HOME/.config/systemd/user/$UNIT"
# Continuous absence before self-removal. Installing/updating Deckback is an uninstall immediately
# followed by an install, so a short grace period makes the helper delete itself mid-update. It is
# inert without Deckback, so waiting costs nothing.
TICK_SECONDS=2
MISSING_GRACE_SECONDS=900
MISSING_LIMIT=$((MISSING_GRACE_SECONDS / TICK_SECONDS))

deckback_installed() {
  compgen -G "$HOME/.local/share/flatpak/app/$APP" >/dev/null 2>&1 ||
    compgen -G "/var/lib/flatpak/app/$APP" >/dev/null 2>&1
}

self_uninstall() {
  # Transient unit = its own cgroup, so stopping this service cannot kill the cleanup mid-way.
  local cleanup="systemctl --user disable --now '$UNIT'; rm -f '$BIN' '$UNIT_PATH'; systemctl --user daemon-reload"
  if command -v systemd-run >/dev/null 2>&1; then
    systemd-run --user --collect --quiet /bin/sh -c "$cleanup" || true
  else
    setsid /bin/sh -c "$cleanup" </dev/null >/dev/null 2>&1 &
  fi
  exit 0
}

missing=0
while :; do
  if deckback_installed; then
    missing=0
  else
    missing=$((missing + 1))
    if [ "$missing" -ge "$MISSING_LIMIT" ]; then
      self_uninstall
    fi
  fi

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
  sleep "$TICK_SECONDS"
done
