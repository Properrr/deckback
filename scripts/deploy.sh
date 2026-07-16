#!/usr/bin/env bash
# Assemble a self-contained content_shell runtime bundle and rsync it to the Deck.
# content_shell needs its resource paks + ICU data + V8 snapshots + locales + ANGLE/SwiftShader
# libs alongside the binary — not just the executable. Debug symbols are stripped and kept host-side.
. "$(dirname "$0")/lib.sh"
host="${1:-${DECK_HOST:-}}"
[ -n "$host" ] || die "no deploy host (pass one or set DECK_HOST)"
DECK_HOST="$host" require_deck
preset="deck"
outdir="$COBALT_TREE/out/${preset}"
bin="$outdir/${COBALT_TARGET}"
[ -x "$bin" ] || die "no built binary at $bin — run 'just build ${preset}' first"

bundle="$outdir/bundle"
info "Assembling runtime bundle at $bundle ..."
rm -rf "$bundle"; mkdir -p "$bundle/locales" "$outdir/symbols"

# Binary: keep unstripped copy host-side for symbolication, ship the stripped one.
cp -a "$bin" "$outdir/symbols/${COBALT_TARGET}.debug"
strip -s "$bin" -o "$bundle/${COBALT_TARGET}"

# Required runtime resources — fail loudly if a core file is missing (build misconfig).
require_files="content_shell.pak icudtl.dat snapshot_blob.bin v8_context_snapshot.bin"
for f in $require_files; do
  [ -f "$outdir/$f" ] || die "missing required runtime file: out/${preset}/$f"
  cp -a "$outdir/$f" "$bundle/"
done
# Optional resources / libs — copy whatever this build produced (official vs component differ).
for f in shell_resources.pak ui_resources_100_percent.pak ui_resources_200_percent.pak resources.pak \
         libEGL.so libGLESv2.so libvk_swiftshader.so libvulkan.so.1 vk_swiftshader_icd.json \
         libGLESv2.so.TOC libEGL.so.TOC; do
  [ -e "$outdir/$f" ] && cp -a "$outdir/$f" "$bundle/"
done
cp -a "$outdir"/locales/*.pak "$bundle/locales/" 2>/dev/null || true

# App config + CDP helper (UA injection / debugging until the launcher owns it).
cp -a "$REPO_ROOT/config" "$bundle/"
cp -a "$REPO_ROOT/scripts/cdp.py" "$bundle/"

# AppStream metainfo, staged at the exact relative path about.cpp probes (candidate #3, after
# $DECKBACK_METAINFO and the Flatpak's /app/share/metainfo). Without it the OSD's About tab renders
# only the version — name/summary/description/features all come from this file — which reads as a
# regression against the Flatpak and was reported as one on 2026-07-16. The dev bundle is not a
# Flatpak, so /app/share/metainfo does not exist here; run.sh cd's to ~/cobalt-yt, which makes this
# relative path resolve. Keep the layout: about.cpp hardcodes 'flatpak/assets/<file>'.
meta_src="$REPO_ROOT/flatpak/assets/io.github.properrr.deckback.metainfo.xml"
[ -f "$meta_src" ] || die "missing $meta_src — the About tab would silently degrade to version-only"
mkdir -p "$bundle/flatpak/assets"
cp -a "$meta_src" "$bundle/flatpak/assets/"

# Launcher (out-of-tree; builds anywhere).
./scripts/launcher.sh build
cp -a "$REPO_ROOT/launcher/build/deckback-launcher" "$bundle/"

sz="$(du -sh "$bundle" | cut -f1)"
info "Bundle ready (${sz}). rsync -> ${host}:~/cobalt-yt/ (port ${DECK_PORT}) ..."
deck_ssh "$host" 'mkdir -p ~/cobalt-yt'
# No --delete: preserve a user-fetched Widevine CDM under ~/cobalt-yt/ (never redistributed by us).
deck_rsync -az "$bundle/" "$host:~/cobalt-yt/"
info "Deployed to ${host}:~/cobalt-yt/ (symbols kept at out/${preset}/symbols/)."
