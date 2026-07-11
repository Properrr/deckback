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
# GPG signing (recommended for a public repo): set DECKBACK_GPG_KEY to a key id in your gpg keyring
# and the repo + .flatpakref/.flatpakrepo are signed and carry the public key. Unsigned otherwise
# (fine for local testing; users would add the remote without gpg verification).
#
# Exit: 0 ok · 2 a shipped input is wrong · 3 environment (no flatpak / no bundle).
. "$(dirname "$0")/lib.sh"

app="io.github.properrr.deckback"
bundle="${1:-${app}.flatpak}"
site="${2:-flatpak/pages-site}"

command -v flatpak >/dev/null 2>&1 || die_env "flatpak not found (needed for build-import-bundle)"
[ -f "$bundle" ] || die_env "bundle not found: $bundle (build with 'just flatpak' or 'just release')"

repo="$site/repo"
rm -rf "$site"; mkdir -p "$repo"

gpgargs=()
if [ -n "${DECKBACK_GPG_KEY:-}" ]; then
  info "Signing with GPG key ${DECKBACK_GPG_KEY}"
  gpgargs=(--gpg-sign="$DECKBACK_GPG_KEY")
fi

info "Importing ${bundle} into ${repo} ..."
flatpak build-import-bundle --update-appstream "${gpgargs[@]}" "$repo" "$bundle"
flatpak build-update-repo --generate-static-deltas --prune "${gpgargs[@]}" "$repo"

# Assemble the static site around the repo.
info "Staging site -> ${site} ..."
cp flatpak/pages/index.html                         "$site/index.html"
cp flatpak/deckback.flatpakrepo                      "$site/deckback.flatpakrepo"
cp flatpak/${app}.flatpakref                         "$site/${app}.flatpakref"
cp flatpak/assets/icons/256.png                      "$site/icon.png"
cp flatpak/assets/branding/readme-banner.png         "$site/banner.png"
mkdir -p "$site/screenshots"
cp flatpak/assets/screenshots/*.png                  "$site/screenshots/"

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

cat >&2 <<EOF

Site staged in ${site}/ :
  index.html · deckback.flatpakrepo · ${app}.flatpakref · repo/ · icon.png · banner.png · screenshots/

Test it locally:
  (cd ${site} && python3 -m http.server 8000)
  flatpak remote-add --user --if-not-exists --from deckback-test http://localhost:8000/deckback.flatpakrepo
  flatpak install --user deckback-test ${app}

Publish it: push ${site}/ to GitHub Pages (the release workflow does this automatically on a
GitHub Release; see RELEASING.md).
EOF
info "publish-repo done."
