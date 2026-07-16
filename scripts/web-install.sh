#!/usr/bin/env bash
# Deckback one-line "smart install" for a new Steam Deck.
#
#   curl -fsSL https://properrr.github.io/deckback/install.sh | bash
#
# Published verbatim to the GitHub-Pages site AS install.sh (scripts/publish-repo.sh stages it there,
# next to deckback.flatpakrepo, steam_shortcuts.py and steam/*.png, so every fetch below resolves).
# It is STANDALONE — it does NOT source scripts/lib.sh, because a new user fetches only this one file.
#
# What it does, in order: add the Deckback update repo + install --user (so self-update works),
# confirm the app got evdev (input) access, then — with Steam OFF — write the "Deckback" Steam
# shortcut + full tile artwork itself (steam_shortcuts.py, using an explicit crc appid so the art
# matches; findings/durable/one-line-install.md). Reversible: the shortcut edit backs up shortcuts.vdf
# and can be undone with `steam_shortcuts.py remove`.
set -euo pipefail

APP=io.github.properrr.deckback
BASE="${DECKBACK_SITE:-https://properrr.github.io/deckback}"
FLATHUB="https://flathub.org/repo/flathub.flatpakrepo"

say() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mnote:\033[0m %s\n' "$*" >&2; }
die() {
  printf '\033[1;31merror:\033[0m %s\n' "$*" >&2
  exit 1
}

# --- 0. This only makes sense on a SteamOS-like host, run from Desktop Mode (Konsole). -------------
command -v flatpak >/dev/null 2>&1 ||
  die "flatpak not found. Run this in SteamOS Desktop Mode: switch to Desktop, open Konsole, paste again."
command -v python3 >/dev/null 2>&1 || die "python3 not found (SteamOS ships it — is this a Steam Deck?)."
host_fp="$(flatpak --version 2>/dev/null | awk '{print $NF}')"
if [ -n "$host_fp" ] && [ "$(printf '%s\n%s\n' 1.16.0 "$host_fp" | sort -V | head -1)" != "1.16.0" ]; then
  die "this host has flatpak ${host_fp}; Deckback needs >= 1.16.0 (where --device=input landed, which
     the gamepad and touch lock depend on). Update SteamOS (3.5+ ships flatpak 1.16.x) and retry."
fi

# --- 1. Repo + install --user (a --user install is what lets the app self-update via the portal). --
say "Adding the runtime + Deckback repos ..."
# The Freedesktop runtime comes from Flathub; --if-not-exists so an existing remote is left alone.
flatpak remote-add --user --if-not-exists flathub "$FLATHUB" >/dev/null 2>&1 || true
flatpak remote-add --user --if-not-exists deckback "$BASE/deckback.flatpakrepo"
say "Installing Deckback (first run also pulls the Freedesktop runtime — a few minutes) ..."
flatpak install --user -y deckback "$APP"

# --- 2. Confirm evdev access. Without 'input', the gamepad + touch lock fail SILENTLY (F1). --------
# Mirrors scripts/lib.sh:flatpak_grants_input — only the devices= line counts; 'all' is a superset.
perms="$(flatpak info --show-permissions "$APP" 2>/dev/null || true)"
case "$(printf '%s\n' "$perms" | sed -n 's/^devices=//p')" in
*input* | *all*) say "Controller (input) access is granted." ;;
*)
  warn "granting controller (input) access ..."
  flatpak override --user --device=input "$APP" ||
    warn "could not grant it; if the gamepad does nothing, run: flatpak override --user --device=input $APP"
  ;;
esac

# --- 3. Fetch the Steam-tile helper + artwork into a scratch dir. ----------------------------------
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
say "Fetching the Steam library helper + tile artwork ..."
curl -fsSL "$BASE/steam_shortcuts.py" -o "$tmp/steam_shortcuts.py"
mkdir -p "$tmp/steam"
for png in capsule header hero logo icon; do
  curl -fsSL "$BASE/steam/${png}.png" -o "$tmp/steam/${png}.png"
done

# --- 4. Steam must be OFF: it caches the shortcut list in memory and rewrites shortcuts.vdf on exit,
# so an edit made while it runs is lost. Don't force-kill (disruptive) — ask, then wait. ------------
if pgrep -x steam >/dev/null 2>&1; then
  warn "Steam is running. It would overwrite the new shortcut when it next exits."
  echo "    Fully quit Steam now (Game Mode: hold the STEAM button > Power > Exit to Desktop), then" >&2
  echo "    come back here. Waiting up to 2 minutes ..." >&2
  for _ in $(seq 1 120); do
    pgrep -x steam >/dev/null 2>&1 || break
    sleep 1
  done
fi
if pgrep -x steam >/dev/null 2>&1; then
  die "Steam is still running, so the library edit would not stick. Deckback IS installed already —
     just fully close Steam and re-run this one-liner (it will skip straight to adding the tile)."
fi

# --- 5. Steam-off: write the shortcut + artwork ourselves (crc appid → art matches). --------------
say "Adding 'Deckback' to your Steam library with full artwork ..."
python3 "$tmp/steam_shortcuts.py" add
python3 "$tmp/steam_shortcuts.py" art --assets "$tmp/steam"

say "Done. Start Steam / return to Game Mode — 'Deckback' is in your Library with artwork."
cat >&2 <<'EOF'

Next steps in Game Mode:
  1. Open Deckback's controller settings and apply the "Deckback" layout.
  2. Controls: A=select · B=back · X=play/pause · LB/RB=seek ±10s · View=captions · L3+R3=touch lock.
  3. DRM (rentals/some originals) is OFF by default; free YouTube needs nothing (docs/SUPPORT.md).
EOF
