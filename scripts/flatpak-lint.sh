#!/usr/bin/env bash
# Static gates on everything we ship in the Flatpak, run WITHOUT an engine, a checkout, or a Deck.
#
#   scripts/flatpak-lint.sh
#
# This exists as its own script so CI can run it on every push. `just flatpak` needs a 9.9 GB
# `out/deck` and downloads ~2 GB of runtime; this needs the pack image and four text files. The
# whole point of a gate is that it runs often enough to catch the regression on the commit that
# caused it.
#
# Exit codes: 0 ok · 2 a shipped file is wrong · 3 no pack image / no manifest.
#
# The two validators are not decorative. `appstreamcli validate` is run with **--no-net**: without it
# the tool resolves every <url> over the network and reports `url-not-reachable`, so the gate would
# fail when GitHub is slow, when the repo is private, and (today) because the project is not
# published yet. A check that fails for reasons unrelated to what it checks gets skipped, and then it
# checks nothing.
. "$(dirname "$0")/lib.sh"

if [ "$#" -gt 0 ]; then
  die_usage "flatpak-lint.sh takes no arguments (got: $*)."
fi

app="io.github.properrr.deckback"
manifest="flatpak/${app}.yml"
[ -f "$manifest" ] || die_env "manifest missing: $manifest"

# --- gates that need no container -------------------------------------------------------------

# The app id may never say YouTube. This is a legal constraint (docs/legal.md), not a style rule, and
# it is cheap enough to assert on every push.
if printf '%s' "$app" | grep -qiE 'youtube|(^|[^a-z])yt([^a-z]|$)'; then
  die_assert "app id '${app}' names YouTube/YT — see docs/legal.md"
fi

# Flathub rejects --no-sandbox, and zypak exists precisely so we never need it. If it ever appears in
# finish-args or in a launch wrapper, the packaging story has quietly changed.
#
# Match ARGUMENTS, not prose: both the manifest and this file explain at length why --no-sandbox is
# absent, and a naive `grep -r -- --no-sandbox` fires on the explanation. A gate that cries wolf on
# its own documentation gets deleted within the week.
nosandbox=$(
  grep -nE '^[[:space:]]*-[[:space:]]*--no-sandbox\b' "$manifest" || true
  grep -rnE '^[^#]*--no-sandbox\b' flatpak/assets/ 2>/dev/null || true
)
if [ -n "$nosandbox" ]; then
  printf '%s\n' "$nosandbox" >&2
  die_assert "--no-sandbox appears in the shipped Flatpak (zypak is the sandbox story)"
fi

# --device=all grants every device class to obtain one. We need exactly evdev.
if grep -qE '^\s*-\s*--device=all\b' "$manifest"; then
  die_assert "--device=all in finish-args: use --device=input"
fi

# The inverse: the three features that depend on evdev (gamepad, FF_RUMBLE haptics, touchscreen
# FF_RUMBLE) all fail *silently* without it. This used to be a post-install `flatpak override`.
grep -qE '^\s*-\s*--device=input\b' "$manifest" ||
  die_assert "finish-args lacks --device=input — gamepad and touch lock would be silent no-ops"

# --device=input is only understood from flatpak 1.16.0 (it landed in 1.15.6). Without
# --require-version, a 1.14 host installs the app and hands the user a dead gamepad.
grep -qE '^\s*-\s*--require-version=1\.16\.' "$manifest" ||
  die_assert "finish-args lacks --require-version=1.16.x, but uses --device=input (needs >=1.16.0)"

# Chromium/X11 wants MIT-SHM.
grep -qE '^\s*-\s*--share=ipc\b' "$manifest" ||
  die_assert "finish-args lacks --share=ipc — Chromium's X11 MIT-SHM path would fall back"

# The metainfo has to actually be installed, or `appstreamcli compose` has nothing to compose and
# the app has no catalogue entry.
grep -q "install -Dm644 ${app}.metainfo.xml" "$manifest" ||
  die_assert "the manifest does not install ${app}.metainfo.xml"

# Every screenshot the metainfo advertises by URL must exist as a committed file in the repo, or the
# software centre shows a broken image and Flathub's linter rejects the submission. The catalogue
# fetches these over the network (so `appstreamcli validate` runs --no-net and cannot see them) — this
# is the check that they were actually rendered and committed. We map each raw.githubusercontent URL
# under flatpak/assets/screenshots/ back to its local path.
metainfo="flatpak/assets/${app}.metainfo.xml"
shots=$(grep -oE 'flatpak/assets/screenshots/[A-Za-z0-9._-]+\.png' "$metainfo" | sort -u || true)
[ -n "$shots" ] ||
  die_assert "metainfo advertises no screenshots — Flathub needs at least one (run flatpak/assets/render-assets.sh)"
for s in $shots; do
  [ -f "$s" ] ||
    die_assert "metainfo references '$s' but the file is missing — run flatpak/assets/render-assets.sh"
done

info "manifest gates ok"

# --- gates that need the pack image ------------------------------------------------------------

"$CONTAINER_ENGINE" image inspect deckback-pack:latest >/dev/null 2>&1 ||
  die_env "no deckback-pack:latest image — run 'docker compose build pack'"

"$CONTAINER_ENGINE" run --rm -v "$PWD/flatpak/assets":/a:ro deckback-pack:latest bash -c '
  set -u
  rc=0
  # --no-net: see the header. --explain so a failure names the rule instead of a code.
  appstreamcli validate --no-net --explain "/a/'"${app}"'.metainfo.xml" || rc=2
  desktop-file-validate "/a/'"${app}"'.desktop" || rc=2
  exit $rc
' || die_assert "a shipped metadata file is invalid (see above)"

info "appstreamcli + desktop-file-validate ok"
info "flatpak lint passed."
