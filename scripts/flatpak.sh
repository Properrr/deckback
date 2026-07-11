#!/usr/bin/env bash
# Phase 8 packaging: stage the prebuilt content_shell runtime, then build the Flatpak in the `pack`
# container (dev image + flatpak-builder). Chromium/Cobalt is never built here — only zypak and the
# out-of-tree launcher compile against the SDK; content_shell ships as a staged prebuilt. Output:
# io.github.properrr.deckback.flatpak at the repo root (install on the Deck with 'just install').
#
# Usage: flatpak.sh [preset]   (default: deck)
# The preset selects which out/<preset> tree is packaged. `release.sh` builds out/release and MUST
# pass it here — this used to be hardcoded to `deck`, so `just release` silently bundled whatever
# stale binary happened to sit in out/deck instead of the gold+ThinLTO build it had just produced.
. "$(dirname "$0")/lib.sh"
require_engine
require_cobalt_checkout

app="io.github.properrr.deckback"
manifest="flatpak/${app}.yml"
[ -f "$manifest" ] || die_env "manifest missing: $manifest"

# 0. Lint before staging 1.2 GB and downloading 2 GB of runtime. `appstreamcli compose` runs at the
#    very END of flatpak-builder, so an invalid metainfo file used to be discovered after the whole
#    build. Fail in four seconds instead.
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
#
#    `bash -c`, never `bash -lc`. A login shell runs ~/.bash_logout on the way out, Debian's ends in a
#    command that fails when /usr/bin/clear_console is absent, and under `set -e` that 1 replaces the
#    real exit status — a green build reported as a failure, or worse (finding F8). It happens not to
#    fire in the trixie pack image today, which is precisely the kind of luck not to depend on.
runtime_version="$(sed -n "s/^runtime-version: *'\([^']*\)'.*/\1/p" "$manifest")"
[ -n "$runtime_version" ] || die_env "cannot read runtime-version from $manifest"
sz="$(du -sh "$stage" | cut -f1)"
info "Building Flatpak in the pack container (runtime ${runtime_version}, staged ${sz}) ..."
# The container body is SINGLE-quoted and its inputs arrive as environment variables. The previous
# form was a double-quoted string full of \$ and \" — every edit to it is a coin flip, and the shell
# does not tell you when you lose.
COBALT_COMMIT="$(deps_pin)" "$CONTAINER_ENGINE" compose run --rm \
  -e DB_APP="$app" -e DB_MANIFEST="$manifest" -e DB_RUNTIME="$runtime_version" pack bash -c '
  set -euo pipefail
  flathub=https://dl.flathub.org/repo/flathub.flatpakrepo
  flatpak --user remote-add --if-not-exists flathub "$flathub"
  flatpak-builder --user --install-deps-from=flathub --disable-rofiles-fuse --force-clean \
    --repo=flatpak/repo flatpak/build-dir "$DB_MANIFEST"

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
