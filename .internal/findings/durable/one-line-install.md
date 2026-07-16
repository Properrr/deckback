# One-line "smart install" for a new Deck (design)

## Status: ★ RESOLVED / SHIPPING (2026-07-16). Hosted one-liner landed, verified end-to-end on-Deck, README leads with it.

The last gate — does Game-Mode Steam launch the crc-appid tile and show its art — **PASSED on
hardware (2026-07-16, OLED)**: the real-library `add`+`art` was written to the live `shortcuts.vdf`
(Steam off), the user returned to Game Mode, and the Deckback tile **both launched** (`/usr/bin/flatpak
run io.github.properrr.deckback`) **and showed the full artwork** (capsule/hero/logo keyed to appid
2214955871). All three OPEN items below are therefore settled: the Exe/LaunchOptions launch, the
Steam-off lifecycle around the edit, and the crc appid being honoured for both launch and art lookup.
README §1 now leads with `curl -fsSL https://properrr.github.io/deckback/install.sh | bash`.

### 2026-07-16 progress
- **`scripts/web-install.sh`** (new) — the standalone `curl|bash` orchestrator, published AS
  `install.sh`. Adds the flathub + `deckback` `--user` remotes, `flatpak install --user`, confirms
  evdev (`input`) access, fetches `steam_shortcuts.py` + the five `steam/*.png`, and — **Steam-off**
  (asks + waits, never force-kills) — runs `add` + `art`. Reversible (`.deckback.bak` + `remove`).
- **`scripts/publish-repo.sh`** now stages `install.sh` + `steam_shortcuts.py` + `steam/*.png` onto
  the Pages site (the release workflow calls this script, so CI publishes them too). URLs in
  `web-install.sh` are kept in sync with the staged paths.
- **On-Deck file-logic smoke PASSED (non-destructive)** — `add` + `art` run against a *copy* of the
  Deck's real `shortcuts.vdf` (32 shortcuts): the Deckback tile is written with
  `Exe=/usr/bin/flatpak`, `LaunchOptions=run io.github.properrr.deckback`, `appid == grid_id`
  (2214955871), all five grid PNGs land, the pre-existing 31 non-Deckback shortcuts are preserved,
  and the old desktop-file Deckback entry is deduped/replaced. This proves the vdf layer on real
  Steam data without touching the library.
- **STILL PENDING** (the three OPEN items below): a real edit to the live `shortcuts.vdf` + a Steam
  restart + a human confirming in **Game Mode** that the crc-appid tile *launches* and *shows its
  art*. Until that passes, the README is NOT flipped to lead with the one-liner.

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

2. **Hosted `install.sh`** (LANDED — `scripts/web-install.sh`, published AS `install.sh`) — the
   curl|bash orchestrator. Adds the flathub + `deckback` `--user` remotes + `flatpak install --user`
   (so auto-update works); verifies the `input` device permission; fetches `steam_shortcuts.py` +
   the five `steam/*.png` from the Pages site into a tmpdir; then, Steam-off (asks + waits, never
   force-kills), `add` the shortcut + `art` the tiles; finally prompts to return to Game Mode.

3. **Publish wiring** (LANDED) — `scripts/publish-repo.sh` (called by the release workflow) stages
   `install.sh`, `steam_shortcuts.py`, and the five `steam/*.png` into the site so the one-liner's
   fetches resolve.

4. **README** (LANDED) — §1 leads with the one-liner ("recommended"); the manual repo/bundle steps
   are the collapsed fallback.

## OPEN items — ALL RESOLVED on-Deck 2026-07-16 (see the ★ status note at the top)

- The shortcut's **`Exe` / `LaunchOptions` / `StartDir`** that actually launch the `--user` flatpak
  in Game Mode (e.g. `Exe="/usr/bin/flatpak"`, `LaunchOptions="run io.github.properrr.deckback"`).
  Wrong values → a tile that doesn't launch (non-destructive: `.deckback.bak` + `remove` recover).
- **Steam lifecycle in Desktop Mode**: closing/reopening Steam (or `steam-launcher.service` in Game
  Mode) cleanly around the vdf edit. Getting this wrong is disruptive, so it is verified on hardware.
- That the crc `appid` we write is honoured by Game-Mode Steam for **both** launch and art lookup.

The vdf layer (`add`) is safe and unit-tested; only these hardware behaviours gate the hosted
one-liner. Do NOT flip the README to lead with the one-liner until the on-Deck smoke passes.
