#!/usr/bin/env bash
# Pure helpers for scripts/check-permissions.sh — the release gate that stops a manifest change from
# silently stranding every installed user. No git, no flatpak, no side effects, so the parsing and
# the set comparison are unit-testable at L0 (tests/harness/test_check_permissions.sh). Sourced by
# both the script and its test.
#
# Why this gate exists (findings/durable/self-update.md): flatpak-portal refuses a self-update whose
# new metadata asks for a permission the INSTALLED build lacks —
#   portal/flatpak-portal.c transaction_ready(): flatpak_context_adds_permissions(old, new)
#   -> "Self update not supported, new version requires new permissions"
# The comparison is against whatever is deployed on the user's machine, not against the previous
# release, and flatpak jumps straight to the newest commit. So ONE added permission strands every
# user installed before it, through every later release, until they update by hand. v0.0.7 added
# --talk-name=org.freedesktop.systemd1 and did exactly that.

# The manifest this gate reads. Single definition so the script and the test cannot drift.
DECKBACK_MANIFEST="flatpak/io.github.properrr.deckback.yml"

# finish_args <manifest-text> — the finish-args list, one per line, comments and indentation
# stripped. Reads from the `finish-args:` key to the next top-level key, taking only `- --flag`
# entries, so a `modules:` block that happens to contain a dash-list is never picked up.
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

# permission_args <manifest-text> — the finish-args that actually widen the sandbox, sorted and
# deduplicated. Excludes the two that carry no permission and so cannot trip flatpak's check:
# --require-version (a host-version floor) and --env (FlatpakContext loads env vars but
# flatpak_context_adds_permissions never compares them). Everything else counts — shares, sockets,
# devices, features, bus policies, filesystems, generic policy. Deliberately over-inclusive: a false
# alarm costs a moment's thought, an omission costs every user their updates.
permission_args() {
  finish_args "$1" | grep -vE '^--(require-version|env)=' | LC_ALL=C sort -u
}

# The talk-name without which an install has no updater at all. A release predating it could never
# self-update whatever its permissions are, so comparing against one is meaningless — it is the
# natural floor for the gate rather than an arbitrary pinned tag.
DECKBACK_SELF_UPDATE_ARG="--talk-name=org.freedesktop.portal.Flatpak"

# can_self_update <manifest-text> — true iff that manifest can self-update through the portal.
can_self_update() {
  permission_args "$1" | grep -qxF -- "$DECKBACK_SELF_UPDATE_ARG"
}

# added_permissions <old-args> <new-args> — entries present in new but not in old, one per line.
# Removals are intentionally NOT reported: dropping a permission is always allowed by flatpak.
added_permissions() {
  LC_ALL=C comm -13 <(printf '%s\n' "$1" | LC_ALL=C sort -u) \
    <(printf '%s\n' "$2" | LC_ALL=C sort -u) | grep -v '^$' || true
}

# metadata_permissions <metadata-text> — the same comparison input, read from a BUILT commit's
# `metadata` keyfile instead of the manifest: one token per granted permission, sorted, so
# added_permissions works on it unchanged. publish-repo.sh uses it to compare the bundle about to be
# published with the commit users actually have (the live repo's head), which also catches a bundle
# built from the wrong tree — the tag-based gate above only ever sees manifests.
#   [Context]              key=a;b;     -> context:key=a  context:key=b   (unset-environment skipped)
#   [Session Bus Policy]   name=talk    -> session-bus:name=talk
#   [System Bus Policy]    name=talk    -> system-bus:name=talk
#   [Policy subsystem]     key=a;b;     -> policy:subsystem.key=a ...
# [Application], [Environment] and extension sections carry no permission and are ignored. Like
# permission_args this over-reports (a talk -> own -> talk round trip reads as an add); a false
# alarm costs a look, an omission costs every installed user their updates.
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
