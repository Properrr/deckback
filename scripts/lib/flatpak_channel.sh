#!/usr/bin/env bash
# Pure helpers for the device flatpak-channel switch (scripts/deck-flatpak.sh). No SSH, no flatpak,
# no side effects — so the decisions that gate "did we keep the user's data?" and "which channel is
# the device on?" are unit-testable at L0 (tests/harness/test_flatpak_channel.sh). Sourced by both
# deck-flatpak.sh and its test.

# The remote a local `.flatpak` bundle install creates, vs the remote we add for the published repo.
# Kept here as the single source of truth for both the script and its test.
DECKBACK_DEV_REMOTE="deckback-origin"
DECKBACK_OFFICIAL_REMOTE="deckback"

# channel_of_origin <origin> — classify an installed app's `Origin:` into the channel the user cares
# about. "dev" = a local bundle we sideloaded; "official" = the published repo; "unknown" = anything
# else (e.g. flathub, or a hand-added remote), which the caller reports rather than guessing about.
channel_of_origin() {
  case "$1" in
  "$DECKBACK_DEV_REMOTE") echo dev ;;
  "$DECKBACK_OFFICIAL_REMOTE") echo official ;;
  "") echo none ;;
  *) echo unknown ;;
  esac
}

# flatpakrepo_url_from_repo <repo-url> — derive the `.flatpakrepo` descriptor URL from the ostree
# repo URL. The published site serves the repo at `<base>/repo/` and the descriptor at
# `<base>/deckback.flatpakrepo`, so we strip a trailing `repo/` (and any slashes) and append the
# descriptor name. Prints "" for an empty input so the caller can fail loudly.
flatpakrepo_url_from_repo() {
  local url="$1"
  [ -n "$url" ] || { echo ""; return; }
  url="${url%/}"          # drop one trailing slash
  url="${url%/repo}"      # drop the repo path segment
  url="${url%/}"          # and any slash it left
  echo "${url}/deckback.flatpakrepo"
}

# data_preserved <before-bytes> <after-bytes> — the "keep user data" guard. The one failure this
# exists to catch is app data that was non-empty before the reinstall and is gone (or empty) after,
# which means the YouTube login was wiped. Preserved iff: there was nothing to keep, or something
# still remains. A non-numeric reading is treated as "cannot prove preservation" -> not preserved.
data_preserved() {
  local before="$1" after="$2"
  # Each field must be a non-empty run of digits, checked SEPARATELY: concatenating them would let
  # an empty reading hide behind a valid one ("" + "100" looks numeric).
  case "$before" in '' | *[!0-9]*) return 1 ;; esac
  case "$after" in '' | *[!0-9]*) return 1 ;; esac
  [ "$before" -eq 0 ] && return 0   # nothing to lose
  [ "$after" -gt 0 ]                # something survived
}
