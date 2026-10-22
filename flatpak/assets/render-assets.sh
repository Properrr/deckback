#!/usr/bin/env bash
# Regenerate the committed raster art (AppStream screenshots, Steam artwork, README banner, PNG icon
# fallbacks) from the SVG sources. Deterministic; re-run only when an SVG source changes.
#
#   flatpak/assets/render-assets.sh
#
# Rasterizer, first found: rsvg-convert (librsvg2-bin), cairosvg (pip; honours $CAIROSVG, e.g. a venv
# python), inkscape.
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
# Not part of the Flatpak: installed into Steam's grid folder for the non-Steam shortcut.
steam="$here/steam"
if [ -d "$steam" ]; then
  render "$steam/capsule.svg" "$steam/capsule.png" 600 900     # portrait library capsule
  render "$steam/hero.svg"    "$steam/hero.png"    1920 620    # hero background banner
  render "$steam/logo.svg"    "$steam/logo.png"    900 400     # transparent logo overlay
  render "$steam/header.svg"  "$steam/header.png"  920 430     # landscape capsule / header
  # `steam_shortcuts.py art` also expects an icon.png here.
  [ -f "$icon_svg" ] && render "$icon_svg" "$steam/icon.png" 256 256
  echo "steam art -> steam/{capsule,hero,logo,header,icon}.png"
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
