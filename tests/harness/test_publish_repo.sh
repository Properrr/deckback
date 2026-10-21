#!/usr/bin/env bash
# L1 rehearsal of scripts/publish-repo.sh (TASKS U1/U2/U3): synthetic bundles, a throwaway signing
# key, a local "live" site and a real `flatpak --user` client in a private FLATPAK_USER_DIR. Checks
# what the release pipeline promises, the way a user's machine would see it:
#   - the first publish continues today's production state (a live head whose parent 404s);
#   - every publish chains onto the live head, and a client one behind updates through the static
#     delta (asserted from the server's request log, not from flatpak's wording);
#   - a client two behind still updates;
#   - re-publishing an older bundle is a recall the client installs as an ordinary update;
#   - a bundle that adds a permission is refused (exit 2), and an unreachable live repo is an error
#     (exit 3), never a silent fresh start.
#
# Needs flatpak, ostree, gpg and python3; prints SKIP and passes without them (the CI lint image has
# neither flatpak nor ostree). No network, no root, nothing outside a temp dir.
# Run: tests/harness/test_publish_repo.sh
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
root="$PWD"

for t in flatpak ostree gpg python3; do
  command -v "$t" >/dev/null 2>&1 || { echo "SKIP test_publish_repo: $t not installed"; exit 0; }
done

pass=0 fail=0
ok() { echo "ok   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1"; fail=$((fail + 1)); }
eq() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1: want '$2' got '$3'"; fi; }
# chain <name> <want-parent> <repo-dir> <commit>: an empty commit or parent fails rather than
# comparing equal to another empty string.
chain() {
  local got
  got="$(ostree rev-parse --repo="$3" "${4}^" 2>/dev/null)"
  if [ -n "$2" ] && [ -n "$4" ] && [ "$got" = "$2" ]; then ok "$1"; else bad "$1: want parent '$2' of '$4', got '$got'"; fi
}
yes_() { if "${@:2}"; then ok "$1"; else bad "$1"; fi; }

T="$(mktemp -d)"
server_pid=""
cleanup() {
  [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null
  GNUPGHOME="$T/gnupg" gpgconf --kill gpg-agent 2>/dev/null
  rm -rf "$T"
}
trap cleanup EXIT

app="io.github.properrr.deckback"
ref="app/${app}/x86_64/master"
export GNUPGHOME="$T/gnupg" FLATPAK_USER_DIR="$T/client"
mkdir -m 700 "$GNUPGHOME"
gpg --batch --quiet --passphrase '' --quick-gen-key "deckback test <test@invalid>" ed25519 sign never
key="$(gpg --list-secret-keys --with-colons | awk -F: '/^sec:/{print $5; exit}')"
gpg --export "$key" >"$T/pub.gpg"

# ---- synthetic releases -------------------------------------------------------------------------
# A large file shared by every version (what a delta saves) plus a small per-version marker.
head -c 3000000 /dev/urandom >"$T/blob"
mkapp() { # mkapp <name> <version> [extra System Bus Policy line]
  local d="$T/app-$1"
  mkdir -p "$d/files/bin" "$d/export"
  cp "$T/blob" "$d/files/blob"
  printf '#!/bin/sh\necho %s\n' "$2" >"$d/files/bin/hello"
  chmod +x "$d/files/bin/hello"
  cat >"$d/metadata" <<EOF
[Application]
name=${app}
runtime=org.freedesktop.Platform/x86_64/25.08
command=hello

[Context]
shared=network;ipc;
sockets=x11;pulseaudio;
devices=dri;input;

[Session Bus Policy]
org.freedesktop.portal.Flatpak=talk

[System Bus Policy]
org.freedesktop.login1=talk
${3:-}
EOF
}
# bundle <name> <version> [extra]: each bundle comes from its own builder repo whose commit has a
# parent there (a dev build) that the bundle does NOT carry and no published repo has — like
# flatpak-builder's local repo in production. So a published commit can only chain onto the live head
# if publish-repo.sh made it so, never by accident of the builder's history.
bundle() {
  local b="$T/builder-$1"
  ostree init --repo="$b" --mode=archive-z2
  mkapp "$1" "dev-build-of-$2"
  flatpak build-export "$b" "$T/app-$1" master >/dev/null
  rm -rf "$T/app-$1"
  mkapp "$1" "$2" "${3:-}"
  flatpak build-export "$b" "$T/app-$1" master >/dev/null
  flatpak build-bundle "$b" "$T/$1.flatpak" "$app" master
}
bundle v0a 0.0.8
bundle v0b 0.0.9
bundle v1 0.1.0
bundle v2 0.1.1
bundle v5 0.1.5 "org.freedesktop.systemd1=talk"

# ---- the "live" site: a static server with a request log ----------------------------------------
mkdir -p "$T/live"
port="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')"
python3 -m http.server --bind 127.0.0.1 --directory "$T/live" "$port" >"$T/http.log" 2>&1 &
server_pid=$!
for _ in $(seq 50); do curl -s -o /dev/null "http://127.0.0.1:$port/" && break; sleep 0.1; done
live="http://127.0.0.1:$port/repo"
deploy() { rm -rf "$T/live"/*; cp -a "$1"/. "$T/live"/; } # what a Pages deploy does
publish() { # publish <bundle> <site> [env...] — runs the real script; returns its exit code
  local b="$1" s="$2"
  shift 2
  env DECKBACK_GPG_KEY="$key" DECKBACK_LIVE_REPO="$live" "$@" \
    bash "$root/scripts/publish-repo.sh" "$T/$b.flatpak" "$T/$s" >"$T/$s.log" 2>&1
}
head_of() { ostree rev-parse --repo="$T/$1/repo" "$ref"; }
client_says() { "$FLATPAK_USER_DIR/app/$app/current/active/files/bin/hello"; }
requests_since() { tail -n +"$(($1 + 1))" "$T/http.log"; }

# Production today: the repo rebuilt from one bundle, so the live head's parent is missing (404).
mkdir -p "$T/site0/repo"
ostree init --repo="$T/site0/repo" --mode=archive-z2
flatpak build-import-bundle --gpg-sign="$key" "$T/site0/repo" "$T/v0b.flatpak" >/dev/null
flatpak build-update-repo --generate-static-deltas --prune --gpg-sign="$key" "$T/site0/repo" >/dev/null
deploy "$T/site0"
h0="$(head_of site0)"

# Two clients on the pre-history release: one follows every release, the other skips one.
for c in client client-old; do
  FLATPAK_USER_DIR="$T/$c" flatpak --user remote-add --gpg-import="$T/pub.gpg" deckback "$live" >/dev/null
  FLATPAK_USER_DIR="$T/$c" flatpak --user install -y --noninteractive --no-deps --no-related \
    deckback "$app" >/dev/null 2>&1
done
eq "client installed the pre-history release" "0.0.9" "$(client_says)"

# ---- the first history-keeping publish continues it ---------------------------------------------
publish v1 site1
eq "first publish over a parent-less live head succeeds" 0 $?
h1="$(head_of site1)"
chain "first publish chains onto the live head" "$h0" "$T/site1/repo" "$h1"
deploy "$T/site1"
mark="$(wc -l <"$T/http.log")"
flatpak --user update -y --noninteractive --no-deps --no-related >/dev/null 2>&1
eq "client one behind updated" "0.1.0" "$(client_says)"
# A from-parent delta is named <from>-<to>; a from-scratch one has no '-' (ostree's base64 never
# uses it), so this cannot be satisfied by the full-size fallback.
yes_ "the update came through the from-parent static delta" \
  grep -qE "/repo/deltas/[^/]+/[^/]+-[^/]+/superblock" <(requests_since "$mark")
yes_ "the 3 MB shared file was not fetched as an object" \
  bash -c "! grep -q '\.filez' <<<\"\$(tail -n +$((mark + 1)) '$T/http.log')\""

# ---- second publish: history kept, a client two behind still updates ----------------------------
publish v2 site2
eq "second publish succeeds" 0 $?
h2="$(head_of site2)"
chain "second publish chains onto the first" "$h1" "$T/site2/repo" "$h2"
yes_ "the published repo still holds the previous release's commit" \
  ostree show --repo="$T/site2/repo" "$h1" >/dev/null 2>&1
deploy "$T/site2"
flatpak --user update -y --noninteractive --no-deps --no-related >/dev/null 2>&1
eq "client updated again" "0.1.1" "$(client_says)"
FLATPAK_USER_DIR="$T/client-old" flatpak --user update -y --noninteractive --no-deps --no-related \
  >/dev/null 2>&1
eq "a client two releases behind (no delta from its commit) still updates" "0.1.1" \
  "$("$T/client-old/app/$app/current/active/files/bin/hello")"

# What RELEASING.md tells a user they can do without us: see the kept history, go back one release,
# and hold a version.
yes_ "remote-info --log lists the previous release" \
  grep -q "$h1" <(flatpak --user remote-info --log deckback "$app" 2>/dev/null)
flatpak --user update -y --noninteractive --no-deps --no-related --commit="$h1" "$app" >/dev/null 2>&1
eq "update --commit goes back one release" "0.1.0" "$(client_says)"
flatpak --user mask "$app" >/dev/null 2>&1
flatpak --user update -y --noninteractive --no-deps --no-related >/dev/null 2>&1
eq "a masked app stays where it is" "0.1.0" "$(client_says)"
flatpak --user mask --remove "$app" >/dev/null 2>&1
flatpak --user update -y --noninteractive --no-deps --no-related >/dev/null 2>&1
eq "unmasked, it updates to the newest again" "0.1.1" "$(client_says)"

# Re-running the workflow for the current tag (a site-only fix, a retried deploy) is a no-op.
publish v2 site2b
eq "re-publishing the live release succeeds" 0 $?
eq "...and makes no new commit" "$h2" "$(head_of site2b)"

# ---- recall: the bad 0.1.1 is replaced by re-publishing 0.1.0 -----------------------------------
publish v1 site3
eq "re-publishing an older bundle succeeds" 0 $?
h3="$(head_of site3)"
yes_ "the recall is a NEW commit, not the old one" test -n "$h3" -a "$h3" != "$h1"
deploy "$T/site3"
flatpak --user update -y --noninteractive --no-deps --no-related >/dev/null 2>&1
eq "the client installed the recall as an ordinary update" "0.1.0" "$(client_says)"
FLATPAK_USER_DIR="$T/client-old" flatpak --user update -y --noninteractive --no-deps --no-related \
  >/dev/null 2>&1
eq "the other client lands on the same recall commit" "$h3" \
  "$(FLATPAK_USER_DIR="$T/client-old" flatpak --user info --show-commit "$app")"

# ---- refusals ----------------------------------------------------------------------------------
publish v5 site5
eq "a bundle that adds a permission is refused as a product error" 2 $?
yes_ "the refusal names the permission" grep -q "system-bus:org.freedesktop.systemd1=talk" "$T/site5.log"
publish v5 site5b ALLOW_PERMISSION_ADD=1
eq "ALLOW_PERMISSION_ADD overrides the refusal" 0 $?

DECKBACK_LIVE_REPO_SAVE="$live"
live="http://127.0.0.1:1/repo"
publish v2 site6
eq "an unreachable live repo is an environment error, not a fresh start" 3 $?
publish v2 site7 DECKBACK_REPO_FRESH=1
eq "DECKBACK_REPO_FRESH starts a new history on purpose" 0 $?
h7="$(head_of site7)"
yes_ "a fresh history has a head and no parent" \
  bash -c "[ -n '$h7' ] && ! ostree rev-parse --repo='$T/site7/repo' '${h7}^' >/dev/null 2>&1"
live="$DECKBACK_LIVE_REPO_SAVE"

echo "---- $pass passed, $fail failed"
[ "$fail" -eq 0 ]
