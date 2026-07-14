#!/usr/bin/env bash
# Build a tagged release and draft it on GitHub. Publishing that draft triggers .github/workflows/
# release.yml, which imports the bundle into the ostree repo and deploys it to GitHub Pages.
#
#   scripts/release.sh <tag>            # tag is vX.Y.Z and MUST match the VERSION file
#
# Prereqs: run this on the build host AFTER `just release-prep <X.Y.Z>` has landed on main and the tag
# exists (see RELEASING.md). Exit: 0 ok · 1 usage · 2 input wrong · 3 environment.
. "$(dirname "$0")/lib.sh"
tag="${1:-}"
[ -n "$tag" ] || die_usage "usage: release.sh <tag>   (e.g. v0.0.1)"

# The tag must match VERSION, or the GitHub release, the repo commit, and the changelog disagree.
ver="${tag#v}"
file_ver="$(tr -d '[:space:]' < VERSION 2>/dev/null)"
[ "$ver" = "$file_ver" ] ||
  die_assert "tag '$tag' does not match VERSION '$file_ver' — run 'just release-prep $ver' first"

# Gate: the tagged commit must be GREEN in CI before we burn a Chromium-scale build on it. 0.0.3
# nearly shipped from a commit whose launcher job was red; a build that takes an hour is the wrong
# place to discover a red tag. Every check-run on the tagged commit must be completed+success.
# Override for an offline/emergency build:  FORCE=1 just release <tag>   (or a --force 2nd arg).
force="${FORCE:-0}"
[ "${2:-}" = "--force" ] && force=1
if [ "$force" = 1 ]; then
  info "FORCE set — skipping the CI-green gate for $tag."
else
  command -v gh >/dev/null 2>&1 || die_env "need 'gh' to verify CI is green on $tag (or FORCE=1 to skip)"
  sha="$(git rev-list -n1 "$tag" 2>/dev/null)" || die_assert "tag $tag not found locally — create/fetch it first"
  info "Verifying CI is green on $tag ($sha) ..."
  states="$(gh api "repos/{owner}/{repo}/commits/$sha/check-runs" --jq '.check_runs[] | "\(.name) \(.status)/\(.conclusion)"' 2>/dev/null)" ||
    die_transport "could not query CI status for $sha (network/gh auth?) — retry, or FORCE=1"
  [ -n "$states" ] || die_assert "no CI check-runs on $sha — has lint run and finished? (FORCE=1 to override)"
  if printf '%s\n' "$states" | grep -qv ' completed/success$'; then
    echo "CI on $sha is not all green:" >&2
    printf '%s\n' "$states" | sed 's/^/  /' >&2
    die_assert "refusing to build a release from a commit that is not green in CI (FORCE=1 to override)"
  fi
  info "CI green on $tag ✓ — building."
fi

info "Release build (gold + ThinLTO) ..."
in_container bash -c "cd $CTR_TREE && python3 cobalt/build/gn.py --no-rbe -p $COBALT_PLATFORM -c gold out/release"
# Same layering as gen.sh (common.gn + deck.gn) plus ThinLTO for the tagged build. Strip
# concurrent_links (ThinLTO manages its own link concurrency and GN asserts against an explicit
# value: "can't explicitly set concurrent_links with thinlto") and deck.gn's use_thin_lto=false,
# which the appended `use_thin_lto = true` below replaces.
{ cat args/common.gn args/deck.gn 2>/dev/null \
    | grep -vE '^\s*#|^\s*$|^\s*concurrent_links\b|^\s*use_thin_lto\b'; echo 'use_thin_lto = true'; } \
  >> "$COBALT_TREE/out/release/args.gn"
in_container bash -c "cd $CTR_TREE && gn gen out/release && autoninja -C out/release $COBALT_TARGET"

# Package the tree we just built, NOT out/deck. Passing the preset is load-bearing: without it
# flatpak.sh defaults to `deck` and bundles a stale binary while reporting success.
info "Flatpak bundle (preset=release) ..."
./scripts/flatpak.sh release

# Engine tarball: the extra-data payload the Flathub manifest references (and a convenient standalone
# artifact). Same flat layout apply_extra expects. Staged by flatpak.sh at flatpak/cobalt-prebuilt/.
pin="$(tr -d '[:space:]' < DEPS.pin)"
engine="cobalt-prebuilt-${pin}.tar.gz"
if [ -d flatpak/cobalt-prebuilt ] && [ -e flatpak/cobalt-prebuilt/content_shell ]; then
  info "Engine tarball -> ${engine} ..."
  tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
      -czf "$engine" -C flatpak/cobalt-prebuilt .
fi

info "Checksums ..."
sha256sum io.github.properrr.deckback.flatpak ${engine:+"$engine"} > "SHA256SUMS-${tag}.txt"

# Release notes: the section for this version from the changelog (everything between this version's
# heading and the next `## [` heading).
notes="$(mktemp)"
awk -v v="## [$ver]" '
  index($0, v)==1 {grab=1; next}
  grab && /^## \[/ {exit}
  grab {print}
' CHANGELOG.md > "$notes"
[ -s "$notes" ] || printf 'Deckback %s\n' "$tag" > "$notes"

if command -v gh >/dev/null 2>&1; then
  info "Drafting GitHub release ${tag} ..."
  gh release create "$tag" --draft --title "Deckback ${tag}" --notes-file "$notes" \
    io.github.properrr.deckback.flatpak "SHA256SUMS-${tag}.txt" ${engine:+"$engine"}
  info "Draft ready. Review it, then PUBLISH to trigger the Pages deploy (release-pages workflow)."
else
  info "gh CLI not found — artifacts ready (bundle, ${engine:-no engine tarball}, checksums, notes at $notes)."
  info "Create the release manually and attach them; publishing triggers the Pages workflow."
fi
