#!/usr/bin/env bash
# Build a tagged release and draft it on GitHub. Publishing that draft triggers .github/workflows/
# release.yml, which imports the bundle into the ostree repo and deploys it to GitHub Pages.
#
#   scripts/release.sh <tag>            # tag is vX.Y.Z and MUST match the VERSION file
#
# Prereqs: run this on the build host AFTER `just release-prep <X.Y.Z>` has landed on main and the tag
# exists (see RELEASING.md).
# Env: FORCE=1 (or a --force 2nd arg) skips the CI-green gate · RELEASE_ENGINE_PRESET (see below).
# Exit: 0 ok · 2 input wrong / CI not green · 3 environment · 4 transport · 5 usage.
. "$(dirname "$0")/lib.sh"
tag="${1:-}"
[ -n "$tag" ] || die_usage "usage: release.sh <tag>   (e.g. v0.0.1)"

# The tag must match VERSION, or the GitHub release, the repo commit and the changelog disagree.
ver="${tag#v}"
file_ver="$(tr -d '[:space:]' < VERSION 2>/dev/null)"
[ "$ver" = "$file_ver" ] ||
  die_assert "tag '$tag' does not match VERSION '$file_ver' — run 'just release-prep $ver' first"

# Gate: every check-run on the tagged commit must be completed+success before the build.
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

# RELEASE_ENGINE_PRESET=<preset>: package the prebuilt engine in out/<preset> instead of building
# one (a release whose DEPS.pin, patches/ and args/ match a previous tag; RELEASING.md §3).
preset="${RELEASE_ENGINE_PRESET:-release}"
if [ "$preset" = release ]; then
  info "Release build (gold + ThinLTO) ..."
  in_container bash -c "cd $CTR_TREE && python3 cobalt/build/gn.py --no-rbe -p $COBALT_PLATFORM -c gold out/release"
  # Same layering as gen.sh (common.gn + deck.gn) plus ThinLTO. Strip concurrent_links (GN asserts
  # against an explicit value with ThinLTO) and deck.gn's use_thin_lto, replaced by the appended line.
  { cat args/common.gn args/deck.gn 2>/dev/null \
      | grep -vE '^\s*#|^\s*$|^\s*concurrent_links\b|^\s*use_thin_lto\b'; echo 'use_thin_lto = true'; } \
    >> "$COBALT_TREE/out/release/args.gn"
  in_container bash -c "cd $CTR_TREE && gn gen out/release && autoninja -C out/release $COBALT_TARGET"
else
  [ -x "$COBALT_TREE/out/$preset/$COBALT_TARGET" ] ||
    die_env "RELEASE_ENGINE_PRESET=$preset but there is no $COBALT_TARGET in out/$preset"
  info "NOT building the engine: packaging the prebuilt one in out/$preset (RELEASE_ENGINE_PRESET)."
fi

info "Flatpak bundle (preset=$preset) ..."
./scripts/flatpak.sh "$preset"

# Engine tarball: the extra-data payload the Flathub manifest references, in the flat layout
# apply_extra expects (staged by flatpak.sh at flatpak/cobalt-prebuilt/).
pin="$(tr -d '[:space:]' < DEPS.pin)"
engine="cobalt-prebuilt-${pin}.tar.gz"
if [ -d flatpak/cobalt-prebuilt ] && [ -e flatpak/cobalt-prebuilt/content_shell ]; then
  info "Engine tarball -> ${engine} ..."
  tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
      -czf "$engine" -C flatpak/cobalt-prebuilt .
fi

info "Checksums ..."
sha256sum io.github.properrr.deckback.flatpak ${engine:+"$engine"} > "SHA256SUMS-${tag}.txt"

# Release notes: this version's CHANGELOG section, up to the next `## [` heading.
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
