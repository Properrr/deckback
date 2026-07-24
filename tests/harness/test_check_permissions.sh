#!/usr/bin/env bash
# L0 tests for scripts/lib/permissions.sh — the parsing and set comparison behind
# scripts/check-permissions.sh, the gate that stops a manifest change stranding installed users.
#
# The load-bearing ones: permission_args() must pick up EVERY sandbox-widening arg (an omission is
# exactly the v0.0.7 bug — one unnoticed --talk-name and no 0.0.6 user could ever self-update again)
# while ignoring the two that carry no permission; added_permissions() must report additions and
# stay silent on removals, which flatpak always allows; and can_self_update() must recognise the
# portal talk-name so releases predating the updater are skipped rather than failed forever.
#
# Needs no git, no flatpak, no container. Run: tests/harness/test_check_permissions.sh
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1

# shellcheck source=scripts/lib/permissions.sh
. scripts/lib/permissions.sh

pass=0 fail=0
ok() { echo "ok   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1"; fail=$((fail + 1)); }
eq() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1: want '$2' got '$3'"; fi; }
code() {
  local n="$1" want="$2"
  shift 2
  "$@"
  local got=$?
  if [ "$got" = "$want" ]; then ok "$n"; else bad "$n: want rc $want got $got"; fi
}

MANIFEST_06='app-id: io.github.properrr.deckback
finish-args:
  - --require-version=1.16.0
  - --share=network
  - --socket=x11                 # trailing comment must not survive
  - --device=input
  - --talk-name=org.freedesktop.portal.Flatpak
  - --env=DECKBACK_COBALT_BIN=/app/bin/cobalt-zypak

modules:
  - name: zypak
    sources:
      - --not-a-finish-arg
'

# v0.0.7: identical plus one talk-name. This is the exact shape of the change that broke self-update.
MANIFEST_07="${MANIFEST_06/  - --device=input/  - --device=input
  - --talk-name=org.freedesktop.systemd1}"

# ---- finish_args: parsing --------------------------------------------------------------------
eq "finish_args: stops at the next top-level key" "" \
  "$(finish_args "$MANIFEST_06" | grep -F -- '--not-a-finish-arg')"
eq "finish_args: strips a trailing comment" "--socket=x11" \
  "$(finish_args "$MANIFEST_06" | grep -F -- '--socket=')"
eq "finish_args: keeps every entry" "6" "$(finish_args "$MANIFEST_06" | wc -l)"

# ---- permission_args: what counts ----------------------------------------------------------------
eq "permission_args: drops --require-version (a host floor, not a permission)" "" \
  "$(permission_args "$MANIFEST_06" | grep -F -- '--require-version')"
eq "permission_args: drops --env (never compared by flatpak)" "" \
  "$(permission_args "$MANIFEST_06" | grep -F -- '--env=')"
eq "permission_args: keeps --share" "--share=network" \
  "$(permission_args "$MANIFEST_06" | grep -F -- '--share=')"
eq "permission_args: keeps --device" "--device=input" \
  "$(permission_args "$MANIFEST_06" | grep -F -- '--device=')"
eq "permission_args: keeps --talk-name" "--talk-name=org.freedesktop.portal.Flatpak" \
  "$(permission_args "$MANIFEST_06" | grep -F -- '--talk-name=')"
eq "permission_args: sorted and deduplicated" \
  "$(permission_args "$MANIFEST_06" | LC_ALL=C sort -u)" "$(permission_args "$MANIFEST_06")"

# ---- added_permissions: the actual verdict -------------------------------------------------------
eq "added: the v0.0.7 regression is reported" "--talk-name=org.freedesktop.systemd1" \
  "$(added_permissions "$(permission_args "$MANIFEST_06")" "$(permission_args "$MANIFEST_07")")"
eq "added: identical manifests add nothing" "" \
  "$(added_permissions "$(permission_args "$MANIFEST_06")" "$(permission_args "$MANIFEST_06")")"
eq "added: a REMOVED permission is not a failure (0.0.7 -> 0.0.8 is allowed)" "" \
  "$(added_permissions "$(permission_args "$MANIFEST_07")" "$(permission_args "$MANIFEST_06")")"

# ---- can_self_update: the floor ------------------------------------------------------------------
code "can_self_update: yes when the portal talk-name is present" 0 can_self_update "$MANIFEST_06"
code "can_self_update: no when it is absent" 1 \
  can_self_update "${MANIFEST_06/  - --talk-name=org.freedesktop.portal.Flatpak/}"

# ---- the shipped manifest is clean ---------------------------------------------------------------
# Guards the gate itself: if the real manifest ever parses to nothing, every comparison above would
# pass vacuously and the gate would wave a broken release through.
eq "shipped manifest parses to a non-empty permission set" "yes" \
  "$([ -n "$(permission_args "$(cat "$DECKBACK_MANIFEST")")" ] && echo yes || echo no)"
eq "shipped manifest can self-update" "yes" \
  "$(can_self_update "$(cat "$DECKBACK_MANIFEST")" && echo yes || echo no)"

echo "---- $pass passed, $fail failed"
[ "$fail" -eq 0 ]
