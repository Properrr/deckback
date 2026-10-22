#!/usr/bin/env bash
# Deckback one-line "smart install" for a new Steam Deck.
#
#   curl -fsSL https://properrr.github.io/deckback/install.sh | bash
#
# Published AS install.sh by scripts/publish-repo.sh, next to every file fetched below. STANDALONE:
# does not source scripts/lib.sh. Adds the update repo + installs --user (so self-update works),
# confirms evdev access, then with Steam off writes the Steam shortcut + artwork (steam_shortcuts.py;
# undo with `steam_shortcuts.py remove`). Env: DECKBACK_SITE overrides the site base URL.
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
     the gamepad depends on). Update SteamOS (3.5+ ships flatpak 1.16.x) and retry."
fi

# --- 1. Repo + install --user (what lets the app self-update via the portal). ------------------------
say "Adding the runtime + Deckback repos ..."
# The Freedesktop runtime comes from Flathub.
flatpak remote-add --user --if-not-exists flathub "$FLATHUB" >/dev/null 2>&1 || true
flatpak remote-add --user --if-not-exists deckback "$BASE/deckback.flatpakrepo"
# An install from another remote (a bundle's "deckback-origin") cannot be switched over in place:
# flatpak refuses with "already installed from remote X", which would abort here under `set -e`.
origin="$(flatpak info --user --show-origin "$APP" 2>/dev/null || true)"
if [ -n "$origin" ] && [ "$origin" != deckback ]; then
  die "Deckback is installed from '$origin', which never receives these updates. Remove it (your
     sign-in and settings are kept): flatpak uninstall --user $APP — then run this again."
fi
say "Installing or updating Deckback (first run also pulls the Freedesktop runtime — a few minutes) ..."
# --or-update: re-running this is also the fix when a release adds a sandbox permission, which the
# in-app self-update cannot accept.
flatpak install --user -y --or-update deckback "$APP"

# --- 2. Confirm evdev access. Without 'input', the gamepad fails silently. -------------
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

# --- 4. Steam must be OFF: it rewrites shortcuts.vdf from memory on exit. Ask, don't force-kill. --
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
  2. Controls: A=select · B=back · X=play/pause · L1/R1=scrub · L2/R2=previous/next chapter · View=captions · Menu=settings (hold Y there to exit).
  3. DRM (rentals/some originals) is OFF by default; free YouTube needs nothing (docs/SUPPORT.md).
EOF
