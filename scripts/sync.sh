#!/usr/bin/env bash
. "$(dirname "$0")/lib.sh"
require_cobalt_checkout
pin="$(deps_pin)"

if [ -n "$(git -C "$COBALT_TREE" status --porcelain 2>/dev/null)" ]; then
  die "cobalt tree is dirty. Commit/stash or 'just patch-refresh' first — sync refuses a dirty tree."
fi

# --no-history: shallow fetch, single revision. We never care about Cobalt's internal git history
# (DEPS.pin is our reproducible pin), and full history is tens of GB. Our own patch commits sit on
# top of the shallow root, so `git format-patch $pin..HEAD` still works (see scripts/patch.sh).
info "gclient sync -r $pin (shallow, --no-history) ..."
in_container bash -lc "cd /src && gclient sync -r '$pin' --no-history"

if [ -s patches/series ] && grep -qv '^\s*#' patches/series 2>/dev/null; then
  info "Re-applying patches/series ..."
  while read -r p; do
    case "$p" in ''|\#*) continue ;; esac
    info "  apply $p"
    git -C "$COBALT_TREE" apply --index "$REPO_ROOT/patches/$p" \
      || die "patch $p failed to apply against $pin — rebase needed."
  done < patches/series
else
  info "No patches to apply (empty series)."
fi
info "Sync complete."
