#!/usr/bin/env bash
. "$(dirname "$0")/lib.sh"
tag="${1:-}"
[ -n "$tag" ] || die_usage "usage: release.sh <tag>"

info "Release build (gold + ThinLTO) ..."
in_container bash -c "cd $CTR_TREE && python3 cobalt/build/gn.py --no-rbe -p $COBALT_PLATFORM -c gold out/release"
# Same layering as gen.sh (common.gn + deck.gn) plus ThinLTO for the tagged build.
{ cat args/common.gn args/deck.gn 2>/dev/null | grep -vE '^\s*#|^\s*$'; echo 'use_thin_lto = true'; } \
  >> "$COBALT_TREE/out/release/args.gn"
in_container bash -c "cd $CTR_TREE && gn gen out/release && autoninja -C out/release $COBALT_TARGET"

# Package the tree we just built, NOT out/deck. Passing the preset is load-bearing: without it
# flatpak.sh defaults to `deck` and bundles a stale binary while reporting success.
info "Flatpak bundle (preset=release) ..."
./scripts/flatpak.sh release

info "Checksums ..."
sha256sum io.github.properrr.deckback.flatpak > "SHA256SUMS-${tag}.txt"

if command -v gh >/dev/null 2>&1; then
  info "Drafting GitHub release ${tag} ..."
  gh release create "$tag" --draft --title "Deckback ${tag}" \
    io.github.properrr.deckback.flatpak "SHA256SUMS-${tag}.txt"
else
  info "gh CLI not found — bundle + checksums ready; create the release manually."
fi
