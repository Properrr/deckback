# One-line "smart install" for a new Deck

## Status: SHIPPING — verified end-to-end on-Deck (OLED, 2026-07-16). README §1 leads with it.

A command a new user runs once in Desktop-Mode Konsole that installs Deckback **and** adds it to the
Steam library **with full tile artwork**:

    curl -fsSL https://properrr.github.io/deckback/install.sh | bash

Verified on hardware: with Steam off, `add` + `art` wrote the shortcut into the live
`shortcuts.vdf`; back in Game Mode the Deckback tile **launched** (`/usr/bin/flatpak run
io.github.properrr.deckback`) and **showed the full artwork** (capsule/hero/logo keyed to appid
2214955871). That settles the `Exe`/`LaunchOptions` that launch a `--user` flatpak, the Steam-off
lifecycle around the edit, and Game-Mode Steam honouring our crc appid for both launch and art. A
non-destructive run against a copy of a real 32-shortcut `shortcuts.vdf` also showed the other 31
shortcuts preserved and an older desktop-file Deckback entry deduped.

## Why Steam must be off

`steamos-add-to-steam` (used by `scripts/install.sh`, the manual path) adds a shortcut but cannot
apply artwork in the same run: Steam assigns the shortcut's `appid` by a scheme our
`crc32(exe+appname)` does not reproduce (`steam_shortcuts.py` docstring), and it keeps
`shortcuts.vdf` in memory, flushing only on exit. Grid art keyed to a guessed id will not match.

The reliable path (the SteamGridDB/boilr/steamgrid convention) is **Steam-off**: with Steam closed,
write the shortcut ourselves with an explicit `appid = crc32(exe+appname) | 0x80000000`, and write
the five grid PNGs named after that same id. On next Steam start both the tile and its art appear.

## Pieces

1. **`steam_shortcuts.py add`** (L0-tested) — writes/replaces the Deckback shortcut in
   `shortcuts.vdf` with `appid = grid_id(exe, appname)` (`Exe=/usr/bin/flatpak`,
   `LaunchOptions=run io.github.properrr.deckback`). Dedups prior Deckback entries, backs up once
   (`.deckback.bak`), writes atomically (temp + rename), refuses while Steam runs (`--force`
   overrides), and creates a fresh `shortcuts.vdf` under `userdata/<id>/config/` when none exists.
   The field set mirrors what Steam writes. `art` keys off `art_id() == grid_id`, so the match is
   exact and Steam-stable. Undo with `steam_shortcuts.py remove` or the `.deckback.bak`.
2. **`scripts/web-install.sh`**, published AS `install.sh` — standalone (does not source
   `scripts/lib.sh`). Refuses a host flatpak older than 1.16.0 (`--device=input`); adds the flathub +
   `deckback` `--user` remotes and runs `flatpak install --user` (so self-update works); confirms
   evdev (`input`) access; fetches `steam_shortcuts.py` + the five `steam/*.png` from the Pages site;
   then, Steam-off (asks and waits, never force-kills), runs `add` + `art`; finally prompts to return
   to Game Mode. `DECKBACK_SITE` overrides the site base URL.
3. **`scripts/publish-repo.sh`** (called by the release workflow) stages `install.sh`,
   `steam_shortcuts.py` and the five `steam/*.png` onto the Pages site, so the one-liner's fetches
   resolve. URLs in `web-install.sh` must match the staged paths.
4. **README** §1 leads with the one-liner; the manual repo/bundle steps are the collapsed fallback.
