#!/usr/bin/env bash
# Switch the Deck's Deckback install between channels, over SSH, WITHOUT losing user data (the
# YouTube sign-in under ~/.var/app/<id>). Three operations:
#
#   scripts/deck-flatpak.sh dev [bundle]   overwrite the install with a locally-built .flatpak
#   scripts/deck-flatpak.sh repo           (re)point the install at the published repo for updates
#   scripts/deck-flatpak.sh status         report which channel the Deck is on
#
# Why this exists: `flatpak install --reinstall <bundle>` FAILS when the installed app came from a
# remote whose gpg-verify differs from the bundle's (the published repo is gpg-verified; a local
# bundle is not), with "GPG verification enabled, but no signatures found". The robust move is
# uninstall-keeping-data then install — but `flatpak uninstall` deletes ~/.var/app unless you are
# careful, and a dev who loses their sign-in on every rebuild stops testing on real accounts. This
# script does the careful version and PROVES the data survived (it measures the data dir before and
# after and fails if it vanished).
#
# Runs from the workstation and drives the Deck over `deck_ssh`/`deck_rsync` (harness §2/§6). It does
# NOT add the Steam shortcut — that needs the Deck's GUI session (`just install`, run on the Deck).
# An existing shortcut keeps working across all three operations because the app id never changes.
#
# Exit codes (harness §1): 0 ok · 2 the install ended up wrong (missing grant, lost data, wrong
# origin) · 3 environment (no bundle, repo unreachable) · 4 transport (Deck/rsync) · 5 usage.
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/lib/flatpak_channel.sh"

app="io.github.properrr.deckback"

# --- device introspection (each a single SSH round trip) ----------------------------------------

installed_origin() { deck_ssh "$DECK_HOST" "flatpak info $app 2>/dev/null | sed -n 's/^ *Origin: *//p'"; }
installed_commit() { deck_ssh "$DECK_HOST" "flatpak info $app 2>/dev/null | sed -n 's/^ *Commit: *//p'"; }
installed_version() { deck_ssh "$DECK_HOST" "flatpak info $app 2>/dev/null | sed -n 's/^ *Version: *//p'"; }
is_installed() { [ -n "$(installed_origin)" ]; }
app_data_bytes() {
  # 0 when the dir is absent; a plain integer otherwise. Used by data_preserved().
  deck_ssh "$DECK_HOST" "du -sb ~/.var/app/$app 2>/dev/null | cut -f1 || true" | grep -oE '^[0-9]+' || echo 0
}

# Verify the install landed where we intended and left the data intact. Shared by dev + repo.
verify_install() {
  local want_origin="$1" data_before="$2"
  is_installed || die_assert "after install, $app is not present — the install did not take"

  local perms
  perms="$(deck_ssh "$DECK_HOST" "flatpak info --show-permissions $app 2>/dev/null" || true)"
  flatpak_grants_input "$perms" ||
    die_assert "installed $app has no 'input' device permission — the gamepad and touch lock would
    silently do nothing. Rebuild the bundle from a manifest with --device=input."

  local origin
  origin="$(installed_origin)"
  [ "$origin" = "$want_origin" ] ||
    die_assert "expected origin '$want_origin' but the install reports '$origin'"

  local data_after
  data_after="$(app_data_bytes)"
  data_preserved "$data_before" "$data_after" ||
    die_assert "user data was NOT preserved (before=${data_before}B after=${data_after}B). The
    YouTube sign-in was wiped — this is the exact failure this script exists to prevent."
  info "user data preserved (${data_before}B -> ${data_after}B)"
}

# Stop a running instance so uninstall/install is not blocked by an in-use deployment.
kill_running() { deck_ssh "$DECK_HOST" "flatpak kill $app" 2>/dev/null || true; }

# --- dev: overwrite with a local bundle ---------------------------------------------------------

do_dev() {
  local bundle="${1:-${app}.flatpak}"
  [ -f "$bundle" ] || die_env "bundle not found: $bundle (build one with 'just flatpak deck')"
  require_deck

  local base; base="$(basename "$bundle")"
  local want_sha; want_sha="$(sha256sum "$bundle" | cut -d' ' -f1)"
  info "shipping $base to $DECK_HOST ..."
  deck_rsync "$bundle" "$DECK_HOST:~/$base" || die_transport "rsync of the bundle to the Deck failed"
  local got_sha
  got_sha="$(deck_ssh "$DECK_HOST" "sha256sum ~/$base | cut -d' ' -f1" || true)"
  [ "$got_sha" = "$want_sha" ] || die_transport "bundle checksum mismatch after transfer (corrupt copy)"

  local data_before; data_before="$(app_data_bytes)"
  kill_running
  if is_installed; then
    # KEEP data: no --delete-data. The reinstall-over-a-gpg-verified-origin path is why we uninstall
    # first rather than `flatpak install --reinstall <bundle>`.
    info "removing the current install (keeping user data) ..."
    deck_ssh "$DECK_HOST" "flatpak uninstall --user -y $app" >/dev/null 2>&1 ||
      die_assert "could not remove the existing install"
  fi
  info "installing the dev bundle ..."
  deck_ssh "$DECK_HOST" "flatpak install --user -y ~/$base" >/dev/null 2>&1 ||
    die_assert "flatpak install of the dev bundle failed (run it by hand on the Deck to see why)"

  verify_install "$DECKBACK_DEV_REMOTE" "$data_before"
  info "on DEV channel now: commit $(installed_commit), version $(installed_version)"
  info "the Steam shortcut is unchanged (same app id) — launch it, or 'just remote-run'."
}

# --- repo: point the device at the published repo -----------------------------------------------

do_repo() {
  require_deck
  local repo_url="${DECKBACK_REPO_URL:-$(sed -n 's/^Url=//p' flatpak/deckback.flatpakrepo | head -1)}"
  [ -n "$repo_url" ] || die_env "no repo Url in flatpak/deckback.flatpakrepo and DECKBACK_REPO_URL unset"
  local ref_url; ref_url="$(flatpakrepo_url_from_repo "$repo_url")"

  # The Deck is the machine that pulls, so probe reachability FROM the Deck.
  info "checking the repo is reachable from the Deck: $ref_url"
  deck_ssh "$DECK_HOST" "curl -sfI --max-time 10 '$ref_url' >/dev/null 2>&1" ||
    die_env "the published repo descriptor is not reachable from the Deck at:
    $ref_url
  Publish it first ('just publish-repo' then host flatpak/pages-site on GitHub Pages), or set
  DECKBACK_REPO_URL to a reachable repo base (e.g. a staging URL) and retry."

  local data_before; data_before="$(app_data_bytes)"
  kill_running

  info "adding the '$DECKBACK_OFFICIAL_REMOTE' remote on the Deck ..."
  deck_ssh "$DECK_HOST" "flatpak remote-add --user --if-not-exists --from '$DECKBACK_OFFICIAL_REMOTE' '$ref_url'" ||
    die_env "could not add the repo remote (is the .flatpakrepo well-formed?)"

  # Changing an installed app's origin means uninstall (keep data) + install from the new remote;
  # flatpak has no in-place origin switch for a --user install.
  if [ "$(installed_origin)" != "$DECKBACK_OFFICIAL_REMOTE" ] && is_installed; then
    info "removing the current install (keeping user data) ..."
    deck_ssh "$DECK_HOST" "flatpak uninstall --user -y $app" >/dev/null 2>&1 ||
      die_assert "could not remove the existing install"
  fi
  info "installing $app from '$DECKBACK_OFFICIAL_REMOTE' ..."
  deck_ssh "$DECK_HOST" "flatpak install --user -y $DECKBACK_OFFICIAL_REMOTE $app" >/dev/null 2>&1 ||
    die_assert "install from the repo failed (is the app published to it yet?)"

  verify_install "$DECKBACK_OFFICIAL_REMOTE" "$data_before"
  info "on OFFICIAL channel now: commit $(installed_commit), version $(installed_version)"
  info "future releases arrive with 'flatpak update --user $app' on the Deck."
}

# --- status -------------------------------------------------------------------------------------

do_status() {
  require_deck
  if ! is_installed; then
    info "$app is not installed on $DECK_HOST."
    exit "$EX_OK"
  fi
  local origin; origin="$(installed_origin)"
  printf '  app       %s\n' "$app" >&2
  printf '  channel   %s\n' "$(channel_of_origin "$origin")" >&2
  printf '  origin    %s\n' "$origin" >&2
  printf '  version   %s\n' "$(installed_version)" >&2
  printf '  commit    %s\n' "$(installed_commit)" >&2
  printf '  user data %sB (~/.var/app/%s)\n' "$(app_data_bytes)" "$app" >&2
  printf '  grant     %s\n' \
    "$(flatpak_grants_input "$(deck_ssh "$DECK_HOST" "flatpak info --show-permissions $app 2>/dev/null")" \
       && echo 'input ok' || echo 'MISSING input — gamepad/touch dead')" >&2
}

# --- dispatch -----------------------------------------------------------------------------------

cmd="${1:-}"; shift || true
case "$cmd" in
dev) do_dev "$@" ;;
repo) do_repo "$@" ;;
status) do_status "$@" ;;
*) die_usage "usage: deck-flatpak.sh <dev [bundle]|repo|status>" ;;
esac
