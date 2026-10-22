#!/usr/bin/env bash
. "$(dirname "$0")/lib.sh"
require_cobalt_checkout
pin="$(deps_pin)"

# Refuse real local edits (tracked or untracked). EOL-only differences don't count: Cobalt commits CRLF
# files its own .gitattributes normalizes, so even a pristine checkout reports them as modified.
if ! git -C "$COBALT_TREE" diff --quiet --ignore-cr-at-eol HEAD -- 2>/dev/null \
    || [ -n "$(git -C "$COBALT_TREE" ls-files --others --exclude-standard | head -1)" ]; then
  die "cobalt tree is dirty. Commit/stash or 'just patch-refresh' first — sync refuses a dirty tree."
fi

# A synced tree is the pin plus one commit per patches/series entry (`git am` below). Any other commit
# on top of the last synced pin would be orphaned by the checkout, so refuse it. Compared by patch-id,
# which ignores commit metadata and line offsets.
synced="$(git -C "$COBALT_TREE" rev-parse -q --verify refs/deckback/synced-pin 2>/dev/null || true)"
if [ -n "$synced" ] && [ "$(git -C "$COBALT_TREE" rev-parse HEAD)" != "$synced" ]; then
  series_ids() {
    while read -r p; do
      case "$p" in '' | \#*) continue ;; esac
      git -C "$COBALT_TREE" patch-id --stable <"$REPO_ROOT/patches/$p" | cut -d' ' -f1
    done <patches/series | sort -u
  }
  head_ids() {
    git -C "$COBALT_TREE" log -p --format='commit %H' "$synced..HEAD" \
      | git -C "$COBALT_TREE" patch-id --stable | cut -d' ' -f1 | sort -u
  }
  if [ -n "$(comm -13 <(series_ids) <(head_ids))" ]; then
    die "cobalt/ has commits on top of the last synced pin ($synced) that patches/series does not" \
      "hold. Export them first ('just patch-new <name>' / 'just patch-refresh'), or, if patches/" \
      "is the newer copy, drop them: git -C cobalt/src reset --hard $synced"
  fi
fi

# The `src` solution is `--unmanaged` (bootstrap.sh) and gclient never moves an unmanaged checkout, so
# check out the pin first, then let gclient sync its DEPS. --force only discards EOL noise (real edits
# were refused above). -D deletes dependency checkouts the new DEPS dropped, which would otherwise
# stay behind as untracked trees. --depth=1 / --no-history: shallow, single revision; patch commits
# sit on the shallow root, so `git format-patch $pin..HEAD` still works.
info "Checking out $pin in src, then gclient sync (shallow, --no-history) ..."
in_container bash -lc "
  set -e
  cd /src/src
  git cat-file -e '$pin^{commit}' 2>/dev/null || git fetch --depth=1 origin '$pin'
  git -c advice.detachedHead=false checkout --force --detach '$pin'
  cd /src && gclient sync -r '$pin' --no-history -D
"
[ "$(git -C "$COBALT_TREE" rev-parse HEAD)" = "$pin" ] \
  || die "src is at $(git -C "$COBALT_TREE" rev-parse --short HEAD) after sync, not the pin $pin"

if [ -s patches/series ] && grep -qv '^\s*#' patches/series 2>/dev/null; then
  info "Re-applying patches/series ..."
  # The container's checkout recorded its own stat data (uid, ctime) in the index; without a refresh
  # `git am` refuses every touched file with "does not match index".
  git -C "$COBALT_TREE" update-index -q --refresh >/dev/null || true
  while read -r p; do
    case "$p" in ''|\#*) continue ;; esac
    info "  apply $p"
    # A fixed committer and committer date: the same pin and series always produce the same HEAD.
    git -C "$COBALT_TREE" -c user.name=deckback-sync -c user.email=deckback-sync@invalid \
      am --quiet --committer-date-is-author-date "$REPO_ROOT/patches/$p" || {
      git -C "$COBALT_TREE" am --abort >/dev/null 2>&1 || true
      die "patch $p failed to apply against $pin — rebase needed."
    }
  done < patches/series
else
  info "No patches to apply (empty series)."
fi
git -C "$COBALT_TREE" update-ref refs/deckback/synced-pin "$pin"
info "Sync complete."
