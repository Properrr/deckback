#!/usr/bin/env bash
# L0 tests for scripts/lib/permissions.sh — the parsing and set comparison behind
# scripts/check-permissions.sh, which fails a release that adds a sandbox permission (installed
# users then cannot self-update). Removals are allowed; flatpak always accepts them.
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

# v0.0.7: identical plus one talk-name.
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
# If the real manifest parsed to nothing, every comparison above would pass vacuously.
eq "shipped manifest parses to a non-empty permission set" "yes" \
  "$([ -n "$(permission_args "$(cat "$DECKBACK_MANIFEST")")" ] && echo yes || echo no)"
eq "shipped manifest can self-update" "yes" \
  "$(can_self_update "$(cat "$DECKBACK_MANIFEST")" && echo yes || echo no)"

# ---- metadata_permissions: the live-repo gate (publish-repo.sh) ---------------------------------
# A built commit's `metadata`, as `ostree cat <commit> /metadata` prints it.
META_08='[Application]
name=io.github.properrr.deckback
runtime=org.freedesktop.Platform/x86_64/25.08
command=deckback

[Context]
shared=network;ipc;
sockets=x11;pulseaudio;
devices=dri;input;
unset-environment=FOO;

[Session Bus Policy]
org.freedesktop.portal.Flatpak=talk
org.freedesktop.ScreenSaver=talk

[System Bus Policy]
org.freedesktop.login1=talk

[Environment]
DECKBACK_COBALT_BIN=/app/bin/cobalt-zypak'

eq "metadata: context lists split into one token each" "context:devices=dri
context:devices=input" "$(metadata_permissions "$META_08" | grep '^context:devices=')"
eq "metadata: bus policies keep their level" "session-bus:org.freedesktop.portal.Flatpak=talk" \
  "$(metadata_permissions "$META_08" | grep 'portal.Flatpak')"
eq "metadata: system bus is its own namespace" "system-bus:org.freedesktop.login1=talk" \
  "$(metadata_permissions "$META_08" | grep '^system-bus:')"
eq "metadata: [Application], [Environment] and unset-environment carry no permission" "" \
  "$(metadata_permissions "$META_08" | grep -E 'name=|runtime=|COBALT|unset-environment|FOO')"
eq "metadata: CRLF does not change the tokens" "$(metadata_permissions "$META_08")" \
  "$(metadata_permissions "$(printf '%s' "$META_08" | sed 's/$/\r/')")"

META_07="${META_08/org.freedesktop.login1=talk/org.freedesktop.login1=talk
org.freedesktop.systemd1=talk}"
eq "metadata: the v0.0.7 regression is reported from built commits too" \
  "system-bus:org.freedesktop.systemd1=talk" \
  "$(added_permissions "$(metadata_permissions "$META_08")" "$(metadata_permissions "$META_07")")"
eq "metadata: a removal is allowed" "" \
  "$(added_permissions "$(metadata_permissions "$META_07")" "$(metadata_permissions "$META_08")")"
eq "metadata: an environment-only change is not a permission change" "" \
  "$(added_permissions "$(metadata_permissions "$META_08")" \
    "$(metadata_permissions "${META_08/cobalt-zypak/cobalt-other}")")"
META_DEV="${META_08/devices=dri;input;/devices=dri;input;all;}"
eq "metadata: a widened device list is reported" "context:devices=all" \
  "$(added_permissions "$(metadata_permissions "$META_08")" "$(metadata_permissions "$META_DEV")")"
eq "metadata: [Policy x] entries count" "policy:subsystem.key=v" \
  "$(metadata_permissions "$META_08
[Policy subsystem]
key=v;" | grep '^policy:')"
eq "metadata: an empty input yields no tokens (the caller must refuse that)" "" \
  "$(metadata_permissions "")"

echo "---- $pass passed, $fail failed"
[ "$fail" -eq 0 ]
