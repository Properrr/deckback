#!/usr/bin/env bash
# Pure helpers (no side effects) for scripts/check-permissions.sh and its L0 test: manifest/metadata
# permission parsing and the set comparison. flatpak-portal refuses a self-update whose metadata adds
# a permission the INSTALLED build lacks, so any added permission strands existing installs.

DECKBACK_MANIFEST="flatpak/io.github.properrr.deckback.yml"

# finish_args <manifest-text> — the `- --flag` entries under the top-level `finish-args:` key, one per
# line, comments and indentation stripped.
finish_args() {
  printf '%s\n' "$1" | awk '
    /^[a-zA-Z_-]+:/ { in_block = ($0 ~ /^finish-args:/) ; next }
    !in_block { next }
    {
      line = $0
      sub(/^[[:space:]]*/, "", line)
      if (line !~ /^-[[:space:]]*--/) next
      sub(/^-[[:space:]]*/, "", line)
      sub(/[[:space:]]+#.*$/, "", line)     # trailing comment
      sub(/[[:space:]]+$/, "", line)
      if (line != "") print line
    }'
}

# permission_args <manifest-text> — the finish-args that widen the sandbox, sorted and deduplicated.
# Drops --require-version and --env, which flatpak's permission check never compares; everything else
# counts (deliberately over-inclusive).
permission_args() {
  finish_args "$1" | grep -vE '^--(require-version|env)=' | LC_ALL=C sort -u
}

# The talk-name without which an install has no updater; releases lacking it are the gate's floor.
DECKBACK_SELF_UPDATE_ARG="--talk-name=org.freedesktop.portal.Flatpak"

# can_self_update <manifest-text> — true iff that manifest can self-update through the portal.
can_self_update() {
  permission_args "$1" | grep -qxF -- "$DECKBACK_SELF_UPDATE_ARG"
}

# added_permissions <old-args> <new-args> — entries in new but not in old, one per line (removals are
# always allowed by flatpak, so they are not reported).
added_permissions() {
  LC_ALL=C comm -13 <(printf '%s\n' "$1" | LC_ALL=C sort -u) \
    <(printf '%s\n' "$2" | LC_ALL=C sort -u) | grep -v '^$' || true
}

# metadata_permissions <metadata-text> — one token per permission granted by a BUILT commit's
# `metadata` keyfile, sorted, in the form added_permissions compares:
#   [Context]              key=a;b;     -> context:key=a  context:key=b   (unset-environment skipped)
#   [Session Bus Policy]   name=talk    -> session-bus:name=talk
#   [System Bus Policy]    name=talk    -> system-bus:name=talk
#   [Policy subsystem]     key=a;b;     -> policy:subsystem.key=a ...
# Other sections carry no permission and are ignored.
metadata_permissions() {
  printf '%s\n' "$1" | awk '
    { sub(/\r$/, "") }
    /^\[/ { sec = $0; next }
    /^[[:space:]]*(#|$)/ { next }
    {
      eq = index($0, "=")
      if (!eq) next
      key = substr($0, 1, eq - 1)
      val = substr($0, eq + 1)
      if (sec == "[Context]") {
        if (key == "unset-environment") next
        prefix = "context:" key "="
      } else if (sec == "[Session Bus Policy]") {
        print "session-bus:" key "=" val
        next
      } else if (sec == "[System Bus Policy]") {
        print "system-bus:" key "=" val
        next
      } else if (sec ~ /^\[Policy [^]]+\]$/) {
        prefix = "policy:" substr(sec, 9, length(sec) - 9) "." key "="
      } else {
        next
      }
      n = split(val, parts, ";")
      for (i = 1; i <= n; i++) if (parts[i] != "") print prefix parts[i]
    }' | LC_ALL=C sort -u
}
