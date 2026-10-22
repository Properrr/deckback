#!/usr/bin/env bash
# Installs the Deckback idle-nudge host helper (keeps the screen on / prevents auto-suspend while a
# video plays). Run ON the Deck, in Desktop Mode. It runs on the host because /dev/uinput is not
# available in the Flatpak sandbox and gamescope ignores sandbox-emulated input for idle purposes.
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
# Seed the liveness heartbeat the app stats (scripts/idle-nudge.py heartbeat_path). The daemon
# refreshes it every poll, so a helper that is installed but fails to start leaves it stale.
heartbeat="$HOME/.var/app/io.github.properrr.deckback/data/deckback/idle-nudge.alive"
mkdir -p "$(dirname "$heartbeat")" && : >"$heartbeat"

install -Dm755 "$here/idle-nudge.py" "$HOME/.local/bin/deckback-idle-nudge"
install -Dm644 "$here/deckback-idle-nudge.service" \
  "$HOME/.config/systemd/user/deckback-idle-nudge.service"
systemctl --user daemon-reload
systemctl --user enable deckback-idle-nudge.service
systemctl --user restart deckback-idle-nudge.service # restart (not just --now) so a reinstall reloads updated code
systemctl --user --no-pager --full status deckback-idle-nudge.service || true
echo
echo "Installed. The helper nudges only while Deckback is playing a video; it does nothing otherwise,"
echo "so the Deck still sleeps normally in menus."
echo
echo "If the screen still dims/sleeps during playback (e.g. after a SteamOS update), diagnose with:"
echo "  journalctl --user -u deckback-idle-nudge -f      # 'playback active — nudging' while a video plays?"
echo "  deckback-idle-nudge --check                      # is the playback inhibitor detected right now?"
echo "  deckback-idle-nudge --force 120                  # nudge for 120s with detection bypassed"
echo
echo "Uninstall:"
echo "  systemctl --user disable --now deckback-idle-nudge.service && rm ~/.local/bin/deckback-idle-nudge ~/.config/systemd/user/deckback-idle-nudge.service"
