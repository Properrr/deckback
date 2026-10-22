#!/usr/bin/env bash
# Pure helpers (no side effects) for scripts/deck-flatpak.sh and its L0 test: install-channel
# classification and the keep-user-data guard.

# The remote a local `.flatpak` bundle install creates, vs the remote we add for the published repo.
DECKBACK_DEV_REMOTE="deckback-origin"
DECKBACK_OFFICIAL_REMOTE="deckback"

# is_dev_origin <origin> — does this origin name denote a sideloaded bundle? flatpak suffixes a digit
# when the origin name is already taken (deckback2-origin, ...), so this matches the whole family.
is_dev_origin() {
  case "$1" in
  "$DECKBACK_DEV_REMOTE") return 0 ;;
  deckback[0-9]*-origin) return 0 ;;
  *) return 1 ;;
  esac
}

# channel_of_origin <origin> — prints dev (sideloaded bundle), official (published repo), none (empty)
# or unknown (any other remote).
channel_of_origin() {
  case "$1" in
  "$DECKBACK_OFFICIAL_REMOTE") echo official ;;
  "") echo none ;;
  *) if is_dev_origin "$1"; then echo dev; else echo unknown; fi ;;
  esac
}

# flatpakrepo_url_from_repo <repo-url> — `<base>/repo/` -> `<base>/deckback.flatpakrepo`; prints ""
# for an empty input.
flatpakrepo_url_from_repo() {
  local url="$1"
  [ -n "$url" ] || { echo ""; return; }
  url="${url%/}"
  url="${url%/repo}"
  url="${url%/}"
  echo "${url}/deckback.flatpakrepo"
}

# data_preserved <before-bytes> <after-bytes> — the keep-user-data guard: true iff there was nothing
# to keep or something remains. A non-numeric reading counts as not preserved.
data_preserved() {
  local before="$1" after="$2"
  # Validate each field separately so an empty reading cannot hide behind a valid one.
  case "$before" in '' | *[!0-9]*) return 1 ;; esac
  case "$after" in '' | *[!0-9]*) return 1 ;; esac
  [ "$before" -eq 0 ] && return 0
  [ "$after" -gt 0 ]
}
