#!/usr/bin/env bash
# Deck-side one-command install. Intended to be run ON the Deck (Desktop Mode konsole).
. "$(dirname "$0")/lib.sh"
bundle="${1:-io.github.properrr.deckback.flatpak}"
[ -f "$bundle" ] || die "bundle not found: $bundle (build with 'just flatpak' or download a release)"

app=io.github.properrr.deckback

# The bundle carries --require-version=1.16.0, so `flatpak install` refuses an older host by itself.
# Say so first, in a sentence a person can act on, rather than letting them read flatpak's.
host_fp="$(flatpak --version 2>/dev/null | awk '{print $NF}')"
if [ -n "$host_fp" ] && [ "$(printf '%s\n%s\n' 1.16.0 "$host_fp" | sort -V | head -1)" != "1.16.0" ]; then
  die_env "this host has flatpak ${host_fp}; Deckback needs >= 1.16.0 (that is where --device=input
  landed, and the gamepad and touchscreen lock both depend on it). SteamOS 3.5+ ships 1.16.x."
fi

info "Installing $bundle ..."
flatpak install --user -y "$bundle"

# evdev access is baked into finish-args as of 2026-07-10 (--device=input, never --device=all). It is
# REQUIRED by Phase 3 input: the gamepad reader and the runtime touchscreen lock (EVIOCGRAB on the
# FTS3528) both need it, and BOTH FAIL SILENTLY WITHOUT IT — the app starts, shows the UI, and simply
# ignores the controller.
#
# This used to be a post-install `flatpak override --device=input ... 2>/dev/null && info || info
# "note: not applied"`. Both branches returned 0. The failure path swallowed flatpak's own error
# message and printed a note, so `just install` reported success and the user got a dead gamepad.
# That is finding F1 in a smaller costume: a check that cannot fail is not a check.
#
# So: verify what was actually installed, and refuse to call it a success if it cannot see /dev/input.
perms="$(flatpak info --show-permissions "$app" 2>/dev/null || true)"
[ -n "$perms" ] || die_env "installed $app but cannot read its permissions — is the install broken?"
if flatpak_grants_input "$perms"; then
  info "evdev access granted (gamepad, haptics, touchscreen lock)"
else
  die_assert "installed $app has no 'input' device permission. The gamepad and the touchscreen
  lock will silently do nothing. Rebuild the bundle with 'just flatpak' from a tree whose manifest
  has --device=input, or grant it manually:
      \$ flatpak override --user --device=input $app"
fi

if command -v steamos-add-to-steam >/dev/null 2>&1; then
  info "Adding to Steam ..."
  desktop_dir="$(flatpak info --show-location "$app" 2>/dev/null || true)"
  desktop_file="${desktop_dir%/}/export/share/applications/${app}.desktop"
  if [ -f "$desktop_file" ]; then
    steamos-add-to-steam "$desktop_file"
  else
    info "could not find the installed desktop entry at $desktop_file"
  fi
else
  info "steamos-add-to-steam not found — add a non-Steam shortcut manually:"
  info "  Target: flatpak run io.github.properrr.deckback"
fi

cat >&2 <<'EOF'
Next steps:
  1. In Game Mode, open the Deckback shortcut's controller settings.
  2. Import the "Deckback" layout (config/steam_input.vdf) or apply the community layout by that
     name, so the standard controls reach the in-app input layer and the rear grips/trackpad map.
  3. Controls: A=select, B=back, X=play/pause, LB/RB=seek -/+10s, View=captions. Press both
     stick-clicks (L3+R3) to lock/unlock the touchscreen at runtime.
  4. DRM (rentals/some originals) is OFF by default; free YouTube needs nothing. To enable it, set
     cdm_url + cdm_sha256 in app.json (see docs/SUPPORT.md). The CDM is never bundled with Deckback.
EOF
