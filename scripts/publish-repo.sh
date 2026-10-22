#!/usr/bin/env bash
# Build the hostable Flatpak (ostree) repo from a .flatpak bundle and stage the GitHub-Pages site
# around it. Local counterpart to .github/workflows/release.yml.
#
#   scripts/publish-repo.sh [bundle] [site-dir]
#     bundle    default: io.github.properrr.deckback.flatpak
#     site-dir  default: flatpak/pages-site   (gitignored; the exact tree Pages will serve)
#
# The repo keeps a linear history: the bundle's tree is committed on top of the live head with a fresh
# timestamp, so updates are small from-parent deltas, old commits stay available for rollback, and
# re-publishing an older tag is a recall. A bundle that adds a sandbox permission is refused (the
# portal would not self-update installed users to it).
#
# Environment:
#   DECKBACK_GPG_KEY        sign commits/summary, inline its public key in the .flatpakref/.flatpakrepo,
#                           and verify the live repo with it. Unsigned otherwise.
#   DECKBACK_LIVE_REPO      the repo to continue (default: the production Pages repo).
#   DECKBACK_REPO_FRESH=1   start a new history. Never implied: an unreachable live repo is an error.
#   DECKBACK_REPO_DEPTH     parents kept in the published repo (default 2; each costs about one app
#                           against Pages' 1 GB site limit).
#   ALLOW_PERMISSION_ADD=1  publish a bundle that adds a permission anyway (see check-permissions.sh).
#
# Pages caches `summary` for 10 minutes; two publishes inside that window can chain onto a stale head.
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
  # A parent missing from the live repo ends the walk; ostree does not treat that as an error.
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
# otherwise keep the bundle's build time (older, for a recalled tag).
info "Committing ${bundle} on top of ${live_head:-nothing} ..."
flatpak build-commit-from --src-repo="$scratch/bundle" --timestamp=NOW --no-update-summary \
  "${gpgargs[@]}" "$repo" "$ref"
published="$(ostree rev-parse --repo="$repo" "$ref")"
if [ -n "$live_head" ] && [ "$published" = "$live_head" ]; then
  # build-commit-from makes no commit when the tree already is the live head.
  info "The bundle's tree is already the live head ${live_head} — no new commit; the site is refreshed"
elif [ -n "$live_head" ]; then
  parent="$(ostree rev-parse --repo="$repo" "${published}^")" ||
    die_assert "the new commit has no parent — the history did not chain"
  [ "$parent" = "$live_head" ] || die_assert "the new commit's parent is ${parent}, not the live head ${live_head}"
  # The delta every up-to-date user downloads. ostree only bsdiffs files below a size cap; raise it
  # so the ~160 MB engine binary is diffed, not shipped whole. build-update-repo keeps this delta.
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

info "Staging site -> ${site} ..."
cp flatpak/pages/index.html                         "$site/index.html"
cp flatpak/deckback.flatpakrepo                      "$site/deckback.flatpakrepo"
cp flatpak/${app}.flatpakref                         "$site/${app}.flatpakref"
cp flatpak/assets/icons/256.png                      "$site/icon.png"
cp flatpak/assets/branding/readme-banner.png         "$site/banner.png"
mkdir -p "$site/screenshots"
cp flatpak/assets/screenshots/*.png                  "$site/screenshots/"

# The curl|bash installer is published AS install.sh, with the Steam-tile helper and grid PNGs it
# fetches. Keep these names in sync with the fetch URLs in scripts/web-install.sh.
cp scripts/web-install.sh                            "$site/install.sh"
cp scripts/steam_shortcuts.py                        "$site/steam_shortcuts.py"
mkdir -p "$site/steam"
cp flatpak/assets/steam/capsule.png flatpak/assets/steam/header.png flatpak/assets/steam/hero.png \
   flatpak/assets/steam/logo.png flatpak/assets/steam/icon.png "$site/steam/"

# Inline the base64 public key over the `# GPGKey=` placeholder so a fresh install verifies signatures.
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

# Assert the staged tree, not the sources: a missing file is a silent 404 until the next Release.
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
