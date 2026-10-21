#!/usr/bin/env bash
# Build the hostable Flatpak (ostree) repo from a .flatpak bundle and stage the full GitHub-Pages
# site next to it, so `https://properrr.github.io/deckback/` serves a repo users can add for
# automatic updates. This is the local/manual counterpart to .github/workflows/release.yml — run it
# to test the site before (or instead of) CI.
#
#   scripts/publish-repo.sh [bundle] [site-dir]
#     bundle    default: io.github.properrr.deckback.flatpak
#     site-dir  default: flatpak/pages-site   (gitignored; the exact tree Pages will serve)
#
# The repo keeps a linear HISTORY (TASKS U1). Every publish mirrors the live repo first, then commits
# the bundle's tree on top of the live head with a fresh timestamp, so:
#   - each release's parent is the commit users actually have, and the static delta from it makes an
#     update a few MB instead of the whole ~77 MB app;
#   - the previous DECKBACK_REPO_DEPTH commits stay downloadable — a client mid-update after a
#     deploy still finds its objects, and a user can roll back with `flatpak update --commit=`;
#   - re-publishing an OLDER tag is a recall: its tree lands as a NEW commit, newer than the bad one,
#     so the portal installs it as an ordinary update (it refuses downgrades). RELEASING.md §Recall.
# Before committing, the bundle's permissions are compared with the live head's (TASKS U3): the
# portal refuses a self-update that adds one, which would strand every installed user.
#
# Environment:
#   DECKBACK_GPG_KEY        sign the commits/summary with this key id, inline its public key in the
#                           .flatpakref/.flatpakrepo, and verify the live repo with it. Unsigned
#                           otherwise (fine for local testing).
#   DECKBACK_LIVE_REPO      the repo to continue (default: the production Pages repo).
#   DECKBACK_REPO_FRESH=1   start a new history instead — the very first publish, or a deliberate
#                           reset. Never implied: an unreachable live repo is an error, because
#                           silently starting over is exactly the full-size, no-rollback state this
#                           replaces.
#   DECKBACK_REPO_DEPTH     parents kept in the published repo (default 2). Each costs roughly one
#                           app's worth of objects against Pages' 1 GB site limit.
#   ALLOW_PERMISSION_ADD=1  publish a bundle that adds a permission anyway (strands every installed
#                           user until they re-run the installer; see check-permissions.sh).
#
# Pages caches `summary` for 10 minutes. Two publishes inside that window can chain the second onto
# a stale head — wait it out (RELEASING.md).
#
# Exit: 0 ok · 2 a shipped input is wrong · 3 environment (no flatpak / no bundle / live repo
# unreachable).
. "$(dirname "$0")/lib.sh"
# shellcheck source=scripts/lib/permissions.sh
. "$(dirname "$0")/lib/permissions.sh"

app="io.github.properrr.deckback"
ref="app/${app}/x86_64/master"
bundle="${1:-${app}.flatpak}"
site="${2:-flatpak/pages-site}"
live_url="${DECKBACK_LIVE_REPO:-https://properrr.github.io/deckback/repo}"
depth="${DECKBACK_REPO_DEPTH:-2}"

command -v flatpak >/dev/null 2>&1 || die_env "flatpak not found (needed for build-import-bundle)"
# flatpak pulls in libostree but NOT the ostree CLI, which we need to init the repo. On Debian/Ubuntu:
# 'sudo apt install ostree'.
command -v ostree >/dev/null 2>&1 || die_env "ostree not found (Debian/Ubuntu: 'sudo apt install ostree')"
[ -f "$bundle" ] || die_env "bundle not found: $bundle (build with 'just flatpak' or 'just release')"
case "$depth" in '' | *[!0-9]*) die_env "DECKBACK_REPO_DEPTH must be a number, got '$depth'" ;; esac

repo="$site/repo"
rm -rf "$site"; mkdir -p "$repo"
# build-import-bundle requires an already-initialised archive-z2 repo; an empty dir errors.
ostree init --repo="$repo" --mode=archive-z2
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

gpgargs=()
if [ -n "${DECKBACK_GPG_KEY:-}" ]; then
  info "Signing with GPG key ${DECKBACK_GPG_KEY}"
  gpgargs=(--gpg-sign="$DECKBACK_GPG_KEY")
fi

# ---- 1. continue the live history -----------------------------------------------------------------
live_head=""
if [ -n "${DECKBACK_REPO_FRESH:-}" ]; then
  info "DECKBACK_REPO_FRESH set — starting a new history (no parent, no delta for existing users)"
else
  if [ -n "${DECKBACK_GPG_KEY:-}" ]; then
    gpg --export "$DECKBACK_GPG_KEY" > "$scratch/live.gpg"
    ostree remote add --repo="$repo" --gpg-import="$scratch/live.gpg" live "$live_url"
  else
    ostree remote add --repo="$repo" --no-gpg-verify live "$live_url"
  fi
  info "Mirroring ${ref} (+${depth} parents) from ${live_url} ..."
  # A parent the live repo no longer has (pruned, or published before history was kept) ends the
  # walk; ostree treats it as the edge of the history, not an error.
  ostree pull --repo="$repo" --mirror --depth="$depth" live "$ref" ||
    die_env "could not mirror ${ref} from ${live_url}. Retry, or set DECKBACK_REPO_FRESH=1 to start a new history on purpose."
  ostree remote delete --repo="$repo" live
  live_head="$(ostree rev-parse --repo="$repo" "$ref")"
  info "Live head: ${live_head}"
fi

# ---- 2. the bundle, in a scratch repo -----------------------------------------------------------
ostree init --repo="$scratch/bundle" --mode=archive-z2
flatpak build-import-bundle --no-update-summary "$scratch/bundle" "$bundle" >/dev/null
new_tree="$(ostree rev-parse --repo="$scratch/bundle" "$ref")" ||
  die_assert "${bundle} does not carry ${ref}"

# ---- 3. the permission gate against what users actually run -------------------------------------
if [ -n "$live_head" ]; then
  old_perms="$(metadata_permissions "$(ostree cat --repo="$repo" "$live_head" /metadata)")"
  new_perms="$(metadata_permissions "$(ostree cat --repo="$scratch/bundle" "$new_tree" /metadata)")"
  # An empty set means the metadata did not parse, and every comparison would pass vacuously.
  [ -n "$old_perms" ] && [ -n "$new_perms" ] ||
    die_assert "could not read the sandbox permissions of the live head or the bundle"
  added="$(added_permissions "$old_perms" "$new_perms")"
  if [ -n "$added" ]; then
    echo "  The bundle adds permissions the live release lacks:" >&2
    while IFS= read -r a; do echo "    $a" >&2; done <<<"$added"
    if [ -z "${ALLOW_PERMISSION_ADD:-}" ]; then
      die_assert "the portal would refuse this update for every installed user (set ALLOW_PERMISSION_ADD=1 only for a deliberate, announced break)"
    fi
    info "ALLOW_PERMISSION_ADD set — publishing a release installed users cannot self-update to"
  else
    info "Permissions: nothing added against the live release ✓"
  fi
fi

# ---- 4. commit forward, the from-parent delta, then prune + the rest ----------------------------
# --timestamp=NOW: flatpak refuses a commit older than the installed one, and build-commit-from would
# otherwise keep the bundle's build time — which for a recalled older tag IS older.
info "Committing ${bundle} on top of ${live_head:-nothing} ..."
flatpak build-commit-from --src-repo="$scratch/bundle" --timestamp=NOW --no-update-summary \
  "${gpgargs[@]}" "$repo" "$ref"
published="$(ostree rev-parse --repo="$repo" "$ref")"
if [ -n "$live_head" ] && [ "$published" = "$live_head" ]; then
  # build-commit-from makes no commit when the tree already IS the live head: re-running the
  # workflow for the current tag (to refresh the site, or after a failed deploy) changes nothing for
  # users, which is right.
  info "The bundle's tree is already the live head ${live_head} — no new commit; the site is refreshed"
elif [ -n "$live_head" ]; then
  parent="$(ostree rev-parse --repo="$repo" "${published}^")" ||
    die_assert "the new commit has no parent — the history did not chain"
  [ "$parent" = "$live_head" ] || die_assert "the new commit's parent is ${parent}, not the live head ${live_head}"
  # The delta every up-to-date user downloads. ostree only bsdiffs files below a size cap, so with
  # flatpak's defaults the ~160 MB engine binary ships WHOLE whenever it changes: the real 0.0.8 ->
  # 0.0.9 delta was 66.7 MB that way and 4.2 MB with bsdiff allowed (40 s, 2.5 GB peak RSS on the
  # publishing machine). build-update-repo below keeps an existing delta and adds the rest.
  info "Generating the delta ${live_head:0:12} -> ${published:0:12} (bsdiff, large files included) ..."
  ostree static-delta generate --repo="$repo" --from="$live_head" --to="$published" \
    --max-bsdiff-size=512 --min-fallback-size=512 >/dev/null
  info "Published ${published} (parent ${live_head})"
else
  info "Published ${published} (new history)"
fi
flatpak build-update-repo --generate-static-deltas --prune --prune-depth="$depth" \
  "${gpgargs[@]}" "$repo"
if [ -n "$live_head" ] && [ "$published" != "$live_head" ]; then
  ostree static-delta list --repo="$repo" | grep -qxF "${live_head}-${published}" ||
    die_assert "the from-parent delta did not survive build-update-repo"
fi

# Assemble the static site around the repo.
info "Staging site -> ${site} ..."
cp flatpak/pages/index.html                         "$site/index.html"
cp flatpak/deckback.flatpakrepo                      "$site/deckback.flatpakrepo"
cp flatpak/${app}.flatpakref                         "$site/${app}.flatpakref"
cp flatpak/assets/icons/256.png                      "$site/icon.png"
cp flatpak/assets/branding/readme-banner.png         "$site/banner.png"
mkdir -p "$site/screenshots"
cp flatpak/assets/screenshots/*.png                  "$site/screenshots/"

# The one-line "smart install" (findings/durable/one-line-install.md): the curl|bash orchestrator is
# published AS install.sh, alongside the Steam-tile helper and the five grid PNGs it fetches at
# runtime. Keep these names in sync with the fetch URLs in scripts/web-install.sh.
cp scripts/web-install.sh                            "$site/install.sh"
cp scripts/steam_shortcuts.py                        "$site/steam_shortcuts.py"
mkdir -p "$site/steam"
cp flatpak/assets/steam/capsule.png flatpak/assets/steam/header.png flatpak/assets/steam/hero.png \
   flatpak/assets/steam/logo.png flatpak/assets/steam/icon.png "$site/steam/"

# If signed, export the public key and inline it (base64) into the ref files so a fresh install
# verifies signatures. `sed` appends after the commented placeholder line.
if [ -n "${DECKBACK_GPG_KEY:-}" ]; then
  key_b64="$(gpg --export "$DECKBACK_GPG_KEY" | base64 -w0)"
  for f in "$site/deckback.flatpakrepo" "$site/${app}.flatpakref"; do
    sed -i "s|^# GPGKey=|GPGKey=${key_b64}|" "$f"
  done
  gpg --export "$DECKBACK_GPG_KEY" > "$site/deckback.gpg"
  info "Signed; public key inlined into ref files + written to deckback.gpg"
else
  info "UNSIGNED repo (set DECKBACK_GPG_KEY to sign for a public deployment)"
fi

# Fail-fast completeness gate. The site is only redeployed on a Release, so a missing file here is a
# silent 404 that survives until the next one (exactly how install.sh 404'd: an older publish-repo.sh
# predating these cp lines shipped a site without it). Assert the staged tree — not the sources — so
# dropping any cp line above is caught here, not by a user's curl.
for f in index.html deckback.flatpakrepo "${app}.flatpakref" install.sh steam_shortcuts.py \
  repo/config icon.png; do
  [ -e "$site/$f" ] || die_assert "staged site is missing $f — the one-line installer would 404 (see scripts/publish-repo.sh)"
done

cat >&2 <<EOF

Site staged in ${site}/ :
  index.html · deckback.flatpakrepo · ${app}.flatpakref · repo/ · icon.png · banner.png · screenshots/
  install.sh · steam_shortcuts.py · steam/ (one-line "smart install")

Test it locally:
  (cd ${site} && python3 -m http.server 8000)
  flatpak remote-add --user --if-not-exists --from deckback-test http://localhost:8000/deckback.flatpakrepo
  flatpak install --user deckback-test ${app}

Publish it: push ${site}/ to GitHub Pages (the release workflow does this automatically on a
GitHub Release; see RELEASING.md).
EOF
info "publish-repo done."
