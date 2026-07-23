#!/usr/bin/env bash
# Remove Deckback and every host-side trace it leaves. Run ON the Deck, in Desktop Mode.
# Flatpak has no uninstall hook that runs host commands, so a plain `flatpak uninstall` strands both
# host helpers and the Steam shortcut. Only ever touches Deckback's own artifacts.
#
# NOT set -e: cleanup must continue past a piece that is already gone.
set -uo pipefail

APP="io.github.properrr.deckback"
here="$(cd "$(dirname "$0")" && pwd)"

usage() {
  cat <<'EOF'
deckback-uninstall — remove Deckback and its host-side traces (run in Desktop Mode).
  (default)   remove the app + both host helpers + the Steam shortcut; KEEP app data (YouTube sign-in)
  --purge     also delete app data (~/.var/app/io.github.properrr.deckback) — a full wipe
  --yes, -y   do not prompt for confirmation
EOF
}

purge=0
assume_yes=0
for arg in "$@"; do
  case "$arg" in
    --purge)   purge=1 ;;
    --yes|-y)  assume_yes=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $arg" >&2; usage >&2; exit 2 ;;
  esac
done

if [ "$purge" -eq 1 ]; then
  echo "Removing Deckback, both host helpers, its Steam shortcut, AND deleting app data"
  echo "(your YouTube sign-in). This cannot be undone."
else
  echo "Removing Deckback, both host helpers, and its Steam shortcut."
  echo "App data (your YouTube sign-in) is KEPT — pass --purge to delete it too."
fi
if [ "$assume_yes" -ne 1 ]; then
  printf 'Continue? [y/N] '
  read -r reply
  case "$reply" in
    y|Y|yes|YES) ;;
    *) echo "aborted."; exit 0 ;;
  esac
fi

# 1) Host helpers: stop, disable, remove unit + binary.
for h in idle-nudge audio-repair; do
  unit="deckback-$h.service"
  if systemctl --user disable --now "$unit" 2>/dev/null; then
    echo "removed service $unit"
  fi
  rm -f "$HOME/.local/bin/deckback-$h" "$HOME/.config/systemd/user/$unit"
done
systemctl --user daemon-reload 2>/dev/null || true

# 2) Steam library shortcut + grid artwork. Steam rewrites shortcuts.vdf on exit, so an edit while it
#    runs is lost — skip with a note rather than force a doomed edit.
if pgrep -x steam >/dev/null 2>&1; then
  echo "note: Steam is running — close it and re-run to remove the 'Deckback' library shortcut + art."
elif [ -f "$here/steam_shortcuts.py" ]; then
  python3 "$here/steam_shortcuts.py" remove --appname Deckback || true
fi

# 3) Any per-app Flatpak override we may have set (historically --device=input; now in the manifest).
flatpak override --user --reset "$APP" 2>/dev/null || true

# 4) The Flatpak itself. Keep data unless --purge, so a reinstall resumes the YouTube login.
if flatpak info "$APP" >/dev/null 2>&1; then
  if [ "$purge" -eq 1 ]; then
    flatpak uninstall -y --delete-data "$APP" 2>/dev/null || flatpak uninstall -y "$APP" 2>/dev/null || true
  else
    flatpak uninstall -y "$APP" 2>/dev/null || true
  fi
  echo "uninstalled $APP"
else
  echo "$APP is not installed (removed host-side leftovers only)."
fi

# 5) App data: delete only on --purge.
if [ "$purge" -eq 1 ]; then
  rm -rf "${HOME:?}/.var/app/$APP"
  echo "deleted app data (~/.var/app/$APP)"
elif [ -d "$HOME/.var/app/$APP" ]; then
  echo "kept app data (~/.var/app/$APP) — a reinstall resumes your sign-in; --purge deletes it."
fi

echo "done."
