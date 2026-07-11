#!/usr/bin/env bash
# Regenerate the raster catalogue art (AppStream screenshots + PNG icon fallbacks) from the SVG
# sources under branding/. The PNGs are committed to the repo — this script only needs re-running
# when the SVG sources or the app icon change, and its output is deterministic.
#
#   flatpak/assets/render-assets.sh
#
# It picks whatever SVG rasterizer is on the box, in order of fidelity:
#   1. rsvg-convert (librsvg)         apt: librsvg2-bin
#   2. cairosvg                       pip: cairosvg   (also honours $CAIROSVG, e.g. a venv python)
#   3. inkscape
#
# Screenshots render at the Steam Deck native panel resolution (1280x800). Icon PNGs are square
# fallbacks for hosts/README use; the Flatpak itself installs the scalable SVG, so these are optional.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
src="$here/branding"
shots="$here/screenshots"
icons="$here/icons"
icon_svg="$here/io.github.properrr.deckback.svg"

mkdir -p "$shots" "$icons"

# --- pick a rasterizer ------------------------------------------------------------------------
render() { # render <in.svg> <out.png> <width> <height>
  local in="$1" out="$2" w="$3" h="$4"
  if command -v rsvg-convert >/dev/null 2>&1; then
    rsvg-convert -w "$w" -h "$h" -o "$out" "$in"
  elif [ -n "${CAIROSVG:-}" ] && "$CAIROSVG" -c "import cairosvg" 2>/dev/null; then
    "$CAIROSVG" -c "import cairosvg,sys; cairosvg.svg2png(url='$in', write_to='$out', output_width=$w, output_height=$h)"
  elif python3 -c "import cairosvg" 2>/dev/null; then
    python3 -c "import cairosvg; cairosvg.svg2png(url='$in', write_to='$out', output_width=$w, output_height=$h)"
  elif command -v inkscape >/dev/null 2>&1; then
    inkscape "$in" --export-type=png --export-filename="$out" -w "$w" -h "$h" >/dev/null 2>&1
  else
    echo "render-assets: no SVG rasterizer found (install librsvg2-bin, cairosvg, or inkscape)" >&2
    exit 3
  fi
}

# --- screenshots (1280x800, Deck native) ------------------------------------------------------
for svg in "$src"/screenshot-*.svg; do
  [ -e "$svg" ] || { echo "render-assets: no screenshot sources in $src" >&2; exit 3; }
  base="$(basename "$svg" .svg)"          # screenshot-01-home
  name="${base#screenshot-}"              # 01-home
  echo "screenshot -> screenshots/${name}.png"
  render "$svg" "$shots/${name}.png" 1280 800
done

# --- Steam library artwork (exact per-file sizes) ---------------------------------------------
# These are NOT part of the Flatpak; scripts/steam-artwork.py installs them into Steam's per-user
# grid folder for the non-Steam shortcut. Sizes follow Steam's capsule/hero/logo conventions.
steam="$here/steam"
if [ -d "$steam" ]; then
  render "$steam/capsule.svg" "$steam/capsule.png" 600 900     # portrait library capsule
  render "$steam/hero.svg"    "$steam/hero.png"    1920 620    # hero background banner
  render "$steam/logo.svg"    "$steam/logo.png"    900 400     # transparent logo overlay
  render "$steam/header.svg"  "$steam/header.png"  920 430     # landscape capsule / header
  echo "steam art -> steam/{capsule,hero,logo,header}.png"
fi

# --- README banner (self-contained dark bg; theme-safe on GitHub) -----------------------------
if [ -f "$src/readme-banner.svg" ]; then
  render "$src/readme-banner.svg" "$src/readme-banner.png" 1280 340
  echo "banner -> branding/readme-banner.png"
fi

# --- icon PNG fallbacks -----------------------------------------------------------------------
if [ -f "$icon_svg" ]; then
  for sz in 512 256 128 64 48; do
    echo "icon      -> icons/${sz}.png"
    render "$icon_svg" "$icons/${sz}.png" "$sz" "$sz"
  done
fi

echo "render-assets: done."
