#!/usr/bin/env bash
# Assemble a Flathub-PR-ready manifest set from the current tree. READ flathub/SUBMISSION.md first —
# Flathub cannot build Cobalt from source, so the engine ships as an extra-data tarball pinned by
# sha256; this script builds that tarball, checksums it, pins the source commit, and fills the
# manifest template's placeholders into flathub/build/ (the repo's own copy stays a template).
#
#   scripts/flathub-prep.sh <release-tag>
#
# Prereqs: a staged engine at flatpak/cobalt-prebuilt/ (produced by `just flatpak`). Output:
#   flathub/build/cobalt-prebuilt-<pin>.tar.gz   <- upload as a GitHub release asset for <release-tag>
#   flathub/build/io.github.properrr.deckback.yaml, flathub.json, apply_extra   <- the PR files
#
# Exit: 0 ok · 2 a shipped input is wrong · 3 environment (missing stage / tag).
. "$(dirname "$0")/lib.sh"

tag="${1:-}"
[ -n "$tag" ] || die_usage "usage: flathub-prep.sh <release-tag>   (e.g. v0.0.1)"

app="io.github.properrr.deckback"
tmpl="flathub/${app}.yaml"
stage="flatpak/cobalt-prebuilt"
out="flathub/build"

[ -f "$tmpl" ] || die_env "manifest template missing: $tmpl"
[ -d "$stage" ] && [ -e "$stage/content_shell" ] ||
  die_env "no staged engine at $stage/content_shell — run 'just flatpak' first"

pin="$(tr -d '[:space:]' < DEPS.pin)"
[ -n "$pin" ] || die_env "DEPS.pin is empty"
commit="$(git rev-parse HEAD)"

mkdir -p "$out"
tarball="$out/cobalt-prebuilt-${pin}.tar.gz"

# The engine tarball extracts FLAT (apply_extra untars in /app/extra, so content_shell lands at
# /app/extra/content_shell — matching flathub/cobalt-zypak.sh). Deterministic-ish: sorted, no mtimes.
info "Packing engine runtime -> ${tarball} ..."
tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
    -czf "$tarball" -C "$stage" .

sha256="$(sha256sum "$tarball" | cut -d' ' -f1)"
size="$(stat -c%s "$tarball")"
info "  sha256=${sha256}  size=${size} bytes  pin=${pin}  commit=${commit}"

# Fill the template placeholders into the PR-ready manifest (template itself stays untouched).
sed -e "s|<COMMIT>|${commit}|g" \
    -e "s|<TAG>|${tag}|g" \
    -e "s|<PIN>|${pin}|g" \
    -e "s|<SHA256>|${sha256}|g" \
    -e "s|<SIZE>|${size}|g" \
    "$tmpl" > "$out/${app}.yaml"

# Sanity: no placeholder survived.
if grep -qE '<(COMMIT|TAG|PIN|SHA256|SIZE)>' "$out/${app}.yaml"; then
  die_assert "a placeholder was left unfilled in $out/${app}.yaml"
fi

cp flathub/flathub.json flathub/apply_extra "$out/"

# Validate the metainfo the manifest installs (same gate as flatpak-lint, no engine needed).
metainfo="flatpak/assets/${app}.metainfo.xml"
if command -v appstreamcli >/dev/null 2>&1; then
  appstreamcli validate --no-net "$metainfo" >/dev/null 2>&1 &&
    info "metainfo validates" || die_assert "metainfo failed appstreamcli validate: $metainfo"
else
  info "note: appstreamcli not found — validate $metainfo before submitting"
fi

cat >&2 <<EOF

Flathub PR set ready in ${out}/ :
  - ${app}.yaml   flathub.json   apply_extra      -> commit these to the flathub fork
  - cobalt-prebuilt-${pin}.tar.gz                 -> upload as a release asset on tag '${tag}'
                                                      at github.com/properrr/deckback/releases

Next (see flathub/SUBMISSION.md):
  1. gh release upload ${tag} ${tarball}
  2. verify the asset URL in ${out}/${app}.yaml resolves
  3. flatpak run org.flatpak.Builder --lint manifest ${out}/${app}.yaml
EOF
info "flathub-prep done."
