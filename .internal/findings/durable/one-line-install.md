# One-line "smart install" for a new Deck (design)

## Status: DESIGN + core landed (2026-07-15). Hosted one-liner assembly + on-Deck smoke PENDING (Deck asleep).

## Goal

A copy-paste command a new user runs once in Desktop-Mode Konsole that installs Deckback **and** puts
it in the Steam library **with full tile artwork**, no manual steps:

    curl -fsSL https://properrr.github.io/deckback/install.sh | bash

## Why the naïve version doesn't work

`install.sh` today (`just install`) adds the shortcut with `steamos-add-to-steam`. That works, but
**artwork can't be applied in the same run**: Steam assigns the shortcut's `appid` by a scheme our
`crc32(exe+appname)` does NOT reproduce (`steam_shortcuts.py` docstring), and it keeps `shortcuts.vdf`
in memory, flushing only on exit — so right after `steamos-add-to-steam` we can neither read the real
appid nor rely on a crc one, and grid art keyed to a guessed id won't match.

The reliable path (proven convention, SteamGridDB/boilr/steamgrid) is **Steam-off**: with Steam
closed, write the shortcut ourselves with an explicit `appid = crc32(exe+appname) | 0x80000000`, and
write the five grid PNGs named after that same id. On next Steam start both the tile and its art
appear. This is what the new `steam_shortcuts.py add` does.

## Pieces

1. **`steam_shortcuts.py add`** (LANDED, L0-tested) — write/replace the Deckback shortcut in
   `shortcuts.vdf` with an explicit `appid = grid_id(exe, appname)`, so `art` (and Steam) name the
   grid files with an id we control. Dedups prior Deckback entries, backs up once
   (`.deckback.bak`), writes atomically (temp + rename), refuses while Steam runs (`--force` to
   override), and creates a fresh `shortcuts.vdf` under `userdata/<id>/config/` when none exists. The
   entry field set mirrors what Steam writes (AppName/Exe/StartDir/icon/LaunchOptions/…/tags). Pairs
   with the existing `art` command, which already keys off `art_id() == grid_id` when no Steam-assigned
   appid is present — writing `appid = grid_id` makes that match exact and Steam-stable.

2. **Hosted `install.sh`** (PENDING on-Deck) — the curl|bash orchestrator, published to the Pages
   site next to the repo. Steps: `flatpak remote-add` the `deckback` repo + `flatpak install --user`
   (so auto-update works); verify the `input` device permission (reuse `install.sh`'s gate); fetch
   `steam_shortcuts.py` + the five `steam/*.png` from the Pages site into a tmpdir; then, Steam-off,
   `add` the shortcut + `art` the tiles; finally relaunch Steam / prompt to return to Game Mode.

3. **Publish wiring** — `scripts/publish-repo.sh` (+ the release workflow) stages `install.sh`,
   `steam_shortcuts.py`, and `flatpak/assets/steam/*.png` into the site so the one-liner's fetches
   resolve.

4. **README** — the one-liner becomes the top "recommended" install; the manual steps stay as the
   fallback.

## OPEN — must be settled on a real Deck before advertising the one-liner

- The shortcut's **`Exe` / `LaunchOptions` / `StartDir`** that actually launch the `--user` flatpak
  in Game Mode (e.g. `Exe="/usr/bin/flatpak"`, `LaunchOptions="run io.github.properrr.deckback"`).
  Wrong values → a tile that doesn't launch (non-destructive: `.deckback.bak` + `remove` recover).
- **Steam lifecycle in Desktop Mode**: closing/reopening Steam (or `steam-launcher.service` in Game
  Mode) cleanly around the vdf edit. Getting this wrong is disruptive, so it is verified on hardware.
- That the crc `appid` we write is honoured by Game-Mode Steam for **both** launch and art lookup.

The vdf layer (`add`) is safe and unit-tested; only these hardware behaviours gate the hosted
one-liner. Do NOT flip the README to lead with the one-liner until the on-Deck smoke passes.
