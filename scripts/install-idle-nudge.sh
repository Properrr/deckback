#!/usr/bin/env bash
# Installs the Deckback idle-nudge host helper (keeps the screen on / prevents auto-suspend while a
# video plays). Run this ON the Deck, in Desktop Mode. Like the audio-repair helper, it lives on the
# host — outside the Flatpak sandbox — because /dev/uinput is not available inside it and gamescope
# ignores sandbox-emulated input for idle purposes (see docs/SUPPORT.md).
set -euo pipefail

python3 -c 'import evdev' 2>/dev/null || {
  echo "error: the python3 'evdev' module is required and was not found." >&2
  echo "       (SteamOS ships it; if this fails, the helper cannot inject the wake nudge.)" >&2
  exit 1
}
[ -w /dev/uinput ] || {
  echo "error: /dev/uinput is not writable by $(id -un). The helper cannot create its input device." >&2
  exit 1
}

here="$(cd "$(dirname "$0")" && pwd)"
install -Dm755 "$here/idle-nudge.py" "$HOME/.local/bin/deckback-idle-nudge"
install -Dm644 "$here/deckback-idle-nudge.service" \
  "$HOME/.config/systemd/user/deckback-idle-nudge.service"
systemctl --user daemon-reload
systemctl --user enable deckback-idle-nudge.service
systemctl --user restart deckback-idle-nudge.service   # restart (not just --now) so a reinstall reloads updated code
systemctl --user --no-pager --full status deckback-idle-nudge.service || true
echo
echo "Installed. The helper nudges only while Deckback is playing a video; it does nothing otherwise,"
echo "so the Deck still sleeps normally in menus. Uninstall:"
echo "  systemctl --user disable --now deckback-idle-nudge.service && rm ~/.local/bin/deckback-idle-nudge ~/.config/systemd/user/deckback-idle-nudge.service"
