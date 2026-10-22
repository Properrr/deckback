#!/usr/bin/env bash
# Stage the prebuilt content_shell runtime, then build the Flatpak in the `pack` container. Only zypak
# and the launcher compile against the SDK. Output: io.github.properrr.deckback.flatpak at the repo
# root (install on the Deck with 'just install').
#
# Usage: flatpak.sh [preset]   (default: deck; the out/<preset> tree to package — release.sh passes it)
. "$(dirname "$0")/lib.sh"
require_engine
require_cobalt_checkout

app="io.github.properrr.deckback"
manifest="flatpak/${app}.yml"
[ -f "$manifest" ] || die_env "manifest missing: $manifest"

# 0. Lint first: `appstreamcli compose` only runs at the very end of flatpak-builder.
info "Linting the manifest and shipped metadata ..."
"$(dirname "$0")/flatpak-lint.sh"

preset="${1:-deck}"
outdir="$COBALT_TREE/out/${preset}"
bin="$outdir/${COBALT_TARGET}"
[ -x "$bin" ] || die_env "no built ${COBALT_TARGET} at $bin — run 'just build ${preset}' first"
info "Packaging preset '${preset}' from ${outdir}"

# 1. Stage the content_shell runtime bundle (same shape as deploy.sh) into flatpak/cobalt-prebuilt/.
#    content_shell resolves paks/ICU/snapshots relative to its own dir, so the whole set ships together.
stage="flatpak/cobalt-prebuilt"
info "Staging ${COBALT_TARGET} runtime -> ${stage} ..."
rm -rf "$stage"
mkdir -p "$stage/locales"
strip -s "$bin" -o "$stage/${COBALT_TARGET}"
for f in content_shell.pak icudtl.dat snapshot_blob.bin v8_context_snapshot.bin; do
  [ -f "$outdir/$f" ] || die_env "missing required runtime file: out/${preset}/$f"
  cp -a "$outdir/$f" "$stage/"
done
for f in shell_resources.pak ui_resources_100_percent.pak ui_resources_200_percent.pak resources.pak \
  libEGL.so libGLESv2.so libvk_swiftshader.so libvulkan.so.1 vk_swiftshader_icd.json \
  libGLESv2.so.TOC libEGL.so.TOC; do
  [ -e "$outdir/$f" ] && cp -a "$outdir/$f" "$stage/"
done
cp -a "$outdir"/locales/*.pak "$stage/locales/" 2>/dev/null || true

# 2. Stage config/ into the assets module dir so it is a single self-contained source.
cp -a config/app.json config/steam_input.vdf flatpak/assets/

# 3. Build + bundle inside the pack container. --install-deps-from pulls Platform+Sdk from flathub
#    (persisted on the /cache/flatpak bind-mount); --disable-rofiles-fuse avoids needing fuse.
#    `bash -c`, never `bash -lc`: a login shell runs Debian's ~/.bash_logout, whose failing last
#    command replaces the real exit status under `set -e`.
runtime_version="$(sed -n "s/^runtime-version: *'\([^']*\)'.*/\1/p" "$manifest")"
[ -n "$runtime_version" ] || die_env "cannot read runtime-version from $manifest"
sz="$(du -sh "$stage" | cut -f1)"
info "Building Flatpak in the pack container (runtime ${runtime_version}, staged ${sz}) ..."
# The container body is single-quoted; its inputs arrive as environment variables.
compose_rootless_args
COBALT_COMMIT="$(deps_pin)" "$CONTAINER_ENGINE" compose run --rm "${COMPOSE_ROOTLESS[@]}" \
  -e DB_APP="$app" -e DB_MANIFEST="$manifest" -e DB_RUNTIME="$runtime_version" pack bash -c '
  set -euo pipefail
  # Build every module in parallel: flatpak-builder otherwise serialises to one job in the pack
  # image, and the manifest zypak module keys its `make -j` off FLATPAK_BUILDER_N_JOBS (default 4).
  export FLATPAK_BUILDER_N_JOBS="$(nproc)"
  flathub=https://dl.flathub.org/repo/flathub.flatpakrepo
  flatpak --user remote-add --if-not-exists flathub "$flathub"
  flatpak-builder --user --jobs="$FLATPAK_BUILDER_N_JOBS" --install-deps-from=flathub \
    --disable-rofiles-fuse --force-clean --repo=flatpak/repo flatpak/build-dir "$DB_MANIFEST"

  # Static deltas turn an update into a delta instead of a full re-download; --prune drops the
  # objects the last --force-clean orphaned. Repo hygiene; neither changes the app.
  flatpak build-update-repo --generate-static-deltas --prune flatpak/repo

  # POST-CONDITION, not a formality. This whole change exists so finish-args can carry
  # --device=input. If a manifest edit, a newer flatpak-builder, or a silently-older pack image ever
  # drops it, the app still builds, still installs, and hands the user a dead gamepad and a
  # touchscreen lock that does nothing at all. So read the sandbox back out of the committed ostree
  # object and refuse to ship one that cannot see /dev/input.
  ref=$(ostree --repo=flatpak/repo rev-parse "app/${DB_APP}/x86_64/master")
  meta=$(ostree --repo=flatpak/repo cat "$ref" /metadata)
  devices=$(printf "%s\n" "$meta" | grep "^devices=" || true)
  case "$devices" in
    *input*) ;;
    *) echo "packaged metadata grants no input device: ${devices:-<none>}" >&2; exit 2 ;;
  esac
  case "$devices" in
    *dri*) ;;
    *) echo "packaged metadata grants no dri device: ${devices:-<none>}" >&2; exit 2 ;;
  esac
  echo "packaged context: $(printf "%s\n" "$meta" | grep -E "^(devices|shared|sockets)=" | tr "\n" " ")"

  # --runtime-repo embeds where the runtime came from, so `flatpak install <bundle>` on a Deck that
  # has never seen Platform//$DB_RUNTIME offers to fetch it rather than failing.
  flatpak build-bundle --runtime-repo="$flathub" flatpak/repo "${DB_APP}.flatpak" "$DB_APP"
' || die_assert "flatpak build failed, or the packaged sandbox is wrong (see above)"
info "Wrote ${app}.flatpak ($(du -h "${app}.flatpak" 2>/dev/null | cut -f1))"
