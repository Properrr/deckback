# Flatpak resources — `flatpak/assets/`

This is the single source of truth for every **human-authored** file that goes into (or describes)
the Deckback Flatpak. The build and release pipeline reuses this directory verbatim on every run —
the manifest (`flatpak/io.github.properrr.deckback.yml`) points its `deckback-assets` module at it,
`scripts/flatpak.sh` stages generated config into it, and `scripts/flatpak-lint.sh` validates it with
no engine, checkout, or Deck required. Edit files **here**; do not edit copies under `build-dir/`,
`repo/`, or `cobalt-prebuilt/` — those are generated and git-ignored.

## What lives here

| Path | Role | Source of truth? |
|------|------|------------------|
| `io.github.properrr.deckback.metainfo.xml` | AppStream catalogue entry (name, summary, screenshots, branding, release notes) | ✅ canonical |
| `io.github.properrr.deckback.desktop` | Desktop entry (launcher name, icon, categories) | ✅ canonical |
| `io.github.properrr.deckback.svg` | App icon — scalable; the Flatpak installs this to `hicolor/scalable` | ✅ canonical |
| `deckback.sh` | `/app/bin/deckback` launch wrapper | ✅ canonical |
| `cobalt-zypak.sh` | `/app/bin/cobalt-zypak` zypak shim | ✅ canonical |
| `branding/*.svg` | SVG **sources** for the screenshots (neutral synthetic art, no third-party UI) | ✅ canonical |
| `render-assets.sh` | Regenerates the raster art below from the SVG sources | ✅ canonical |
| `screenshots/*.png` | Rendered catalogue screenshots (1280×800, Deck-native). Referenced by URL from the metainfo | ⚙️ generated, **committed** |
| `icons/*.png` | Square PNG icon fallbacks for README/host use (the Flatpak itself uses the SVG) | ⚙️ generated, **committed** |
| `app.json`, `steam_input.vdf` | Copied in from `config/` at build time by `scripts/flatpak.sh` | 🚫 generated, git-ignored — edit the originals in `config/` |

## Screenshots

The screenshots are **neutral synthetic art**: they depict Deckback's own shell chrome (home screen,
controller-map card, feature grid) over generic tiles, and deliberately show **no** YouTube/Leanback
UI or trademark. Regenerate them from the SVG sources whenever the branding changes:

```sh
flatpak/assets/render-assets.sh          # needs one of: librsvg2-bin, cairosvg, or inkscape
```

The PNGs are **committed** because the AppStream catalogue fetches them over the network — the
metainfo references them at
`https://raw.githubusercontent.com/properrr/deckback/main/flatpak/assets/screenshots/*.png`, so they
must be pushed to `main` for the images to resolve in a software centre. `scripts/flatpak-lint.sh`
fails if the metainfo advertises a screenshot whose file is missing.

## Cutting a release

1. Add a `<release>` entry (version + date + notes) to the top of the `<releases>` list in the
   metainfo. Keep it in sync with the tag `just release <tag>` will create.
2. If the icon or any `branding/*.svg` changed, rerun `render-assets.sh` and commit the updated PNGs.
3. Run `just flatpak-lint` — it validates the metainfo (`appstreamcli`), the desktop file, the
   sandbox arguments, and that every advertised screenshot exists. No engine needed.
4. Push to `main` (so the screenshot URLs resolve), then `just flatpak` / `just release <tag>`.

## Why the app never says "YouTube"

The app id, name, and all shipped metadata must never contain "YouTube"/"YT" — this is a legal
constraint (`docs/legal.md`), enforced by a gate in `scripts/flatpak-lint.sh`. The screenshots follow
the same rule.
