#!/usr/bin/env bash
# L0 tests for scripts/lib/flatpak_channel.sh — the pure decisions behind `just deck-install-dev` /
# `deck-use-repo` / `deck-channel` (scripts/deck-flatpak.sh).
#
# The load-bearing one is data_preserved(): it is the gate that turns "the reinstall wiped the
# YouTube sign-in" from a silent surprise into a failed run (harness §6.4 "verify what you claim").
# A gate that cannot fail is not a gate, so the wiped-data case is asserted explicitly here.
#
# Needs no Deck, no flatpak, no container. Run: tests/harness/test_flatpak_channel.sh
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1

# shellcheck source=scripts/lib/flatpak_channel.sh
. scripts/lib/flatpak_channel.sh

pass=0 fail=0
ok()   { echo "ok   $1"; pass=$((pass + 1)); }
bad()  { echo "FAIL $1"; fail=$((fail + 1)); }

# eq <name> <want> <got>
eq() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1: want '$2' got '$3'"; fi; }
# code <name> <want-rc>  (runs the remaining args, compares $?)
code() { local n="$1" want="$2"; shift 2; "$@"; local got=$?; if [ "$got" = "$want" ]; then ok "$n"; else bad "$n: want rc $want got $got"; fi; }

# ---- channel_of_origin --------------------------------------------------------------------------
eq  "channel: dev bundle origin"      dev      "$(channel_of_origin deckback-origin)"
eq  "channel: official repo origin"   official "$(channel_of_origin deckback)"
eq  "channel: not installed"          none     "$(channel_of_origin '')"
eq  "channel: flathub is unknown"     unknown  "$(channel_of_origin flathub)"
eq  "channel: stray remote unknown"   unknown  "$(channel_of_origin some-other-remote)"

# ---- flatpakrepo_url_from_repo ------------------------------------------------------------------
eq  "url: strips /repo/ and appends descriptor" \
    "https://properrr.github.io/deckback/deckback.flatpakrepo" \
    "$(flatpakrepo_url_from_repo https://properrr.github.io/deckback/repo/)"
eq  "url: tolerates missing trailing slash" \
    "https://properrr.github.io/deckback/deckback.flatpakrepo" \
    "$(flatpakrepo_url_from_repo https://properrr.github.io/deckback/repo)"
eq  "url: staging override with a port" \
    "http://192.168.1.5:8000/deckback.flatpakrepo" \
    "$(flatpakrepo_url_from_repo http://192.168.1.5:8000/repo/)"
eq  "url: empty in -> empty out"      ""       "$(flatpakrepo_url_from_repo '')"

# ---- data_preserved (the guard that keeps the sign-in) ------------------------------------------
code "data: something survived a reinstall"      0 data_preserved 48000000 46000000
code "data: nothing to keep (fresh device)"      0 data_preserved 0 0
code "data: grew after login is fine"            0 data_preserved 0 12000000
code "data: WIPED (non-empty -> empty) FAILS"    1 data_preserved 46000000 0
code "data: non-numeric reading cannot pass"     1 data_preserved 46000000 "?"
code "data: empty reading cannot pass"           1 data_preserved "" 100

echo "---"
echo "flatpak_channel: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
