#!/usr/bin/env bash
# Refuse a manifest that adds a sandbox permission any released version lacks. flatpak-portal refuses
# a self-update that adds permissions relative to the INSTALLED build, so the tree must add nothing
# relative to EVERY release still in the field (see scripts/lib/permissions.sh).
#
# Usage:  scripts/check-permissions.sh [ref ...]      (default: every v* tag)
#
# ALLOW_PERMISSION_ADD=1 downgrades the failure to a warning. Users on older releases must then re-run
# the one-line installer, which updates from the host (`flatpak install --user -y`), not the portal.
#
# Exit: 0 no additions · 2 a permission was added (the tree is wrong) · 3 environment (not a git
# repo / manifest missing).
. "$(dirname "$0")/lib.sh"
# shellcheck source=scripts/lib/permissions.sh
. "$(dirname "$0")/lib/permissions.sh"

command -v git >/dev/null 2>&1 || die_env "git not found"
[ -f "$DECKBACK_MANIFEST" ] || die_env "manifest not found: $DECKBACK_MANIFEST (run from the repo root)"

new_args="$(permission_args "$(cat "$DECKBACK_MANIFEST")")"
[ -n "$new_args" ] || die_assert "no finish-args parsed from $DECKBACK_MANIFEST — the gate would pass vacuously"

refs=("$@")
if [ "${#refs[@]}" -eq 0 ]; then
  mapfile -t refs < <(git tag -l 'v*' | LC_ALL=C sort -V)
fi
if [ "${#refs[@]}" -eq 0 ]; then
  # A shallow clone without tags must not pass vacuously.
  [ "$(git rev-parse --is-shallow-repository 2>/dev/null)" = "true" ] &&
    die_env "no v* tags in a shallow clone — fetch tags before running this gate (CI: actions/checkout with fetch-depth: 0)"
  info "check-permissions: no release tags yet — nothing to compare against"
  exit 0
fi

stranded=0
for ref in "${refs[@]}"; do
  old_manifest="$(git show "${ref}:${DECKBACK_MANIFEST}" 2>/dev/null)" || {
    echo "  skip  $ref (no $DECKBACK_MANIFEST at that ref)" >&2
    continue
  }
  if ! can_self_update "$old_manifest"; then
    echo "  skip  $ref (predates self-update — no updater to strand)" >&2
    continue
  fi
  added="$(added_permissions "$(permission_args "$old_manifest")" "$new_args")"
  if [ -z "$added" ]; then
    echo "  ok    $ref" >&2
    continue
  fi
  stranded=1
  echo "  FAIL  $ref would be stranded — this tree adds:" >&2
  while IFS= read -r a; do echo "          $a" >&2; done <<<"$added"
done

if [ "$stranded" -eq 0 ]; then
  info "check-permissions: no permission added against any release ✓"
  exit 0
fi

cat >&2 <<EOF

A user running one of the releases above CANNOT self-update to this build: flatpak-portal will
refuse the deploy with "Self update not supported, new version requires new permissions", and the
app will stay on its current version until the user updates by hand in Desktop Mode.

Fix it by not needing the permission. The two routes that cost nothing:
  - use a portal we already hold (org.freedesktop.portal.*), or
  - let a host-side helper (deckback-idle-nudge / deckback-audio-repair) do the privileged part and
    hand the result over through ~/.var/app/<id>/, which needs no permission at all.

If the addition is genuinely unavoidable, re-run with ALLOW_PERMISSION_ADD=1 and say so in the
release notes, telling users to re-run the one-line installer once.
EOF

if [ -n "${ALLOW_PERMISSION_ADD:-}" ]; then
  info "check-permissions: ALLOW_PERMISSION_ADD set — proceeding with stranded releases"
  exit 0
fi
exit "$EX_ASSERT"
