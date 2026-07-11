#!/usr/bin/env bash
. "$(dirname "$0")/lib.sh"

# Measured 2026-07-08 on the m114 pin: `--no-history` tree 22 GB, +9.9 GB for one `out/<preset>`,
# ~0.8 GB sccache, ~3 GB flatpak cache, ~6 GB docker images => ~45 GB for a single-preset workstation.
# The old "100-150 GB" figure assumed a full-history checkout; it is ~3x too high for our shallow sync.
info "Sanity check: shallow Cobalt checkout + one build preset wants ~50 GB free disk."
avail_gb="$(df -Pk "$REPO_ROOT" | awk 'NR==2{print int($4/1048576)}')"
[ "${avail_gb:-0}" -ge 80 ] || info "WARNING: only ${avail_gb} GB free here — Chromium may not fit."

pin="$(deps_pin)"
require_engine

# Pre-create the host bind-mount dirs (docker-compose.yml: /cache/sccache, /cache/flatpak). If they do
# not exist, Docker creates them as root:root, and the container's uid-1000 `dev` user then cannot
# write them — sccache silently misses every compile, and `just flatpak` dies with
# "mkdir(/cache/flatpak/share): Permission denied". Creating them as the host user (uid 1000) fixes both.
mkdir -p "${SCCACHE_HOST_DIR:-./.sccache}" "${FLATPAK_HOST_DIR:-./.flatpak-cache}"

info "Building dev container image at commit $pin ..."
COBALT_COMMIT="$pin" "$CONTAINER_ENGINE" compose build dev

if [ ! -d "$COBALT_TREE/.git" ]; then
  info "gclient config + first sync into $COBALT_TREE (this is the long one) ..."
  mkdir -p "$COBALT_SRC"
  # Chrobalt is a Chromium fork: its DEPS is keyed under `src/`, so the solution MUST be named `src`
  # (tree lands at $COBALT_SRC/src). We run gclient from the gclient root (/src) so the managed
  # solution dir (/src/src) is not itself a bind-mount point — this avoids gclient's "conflicting
  # directory -> _bad_scm" move. --no-history keeps it a shallow single-revision fetch (DEPS.pin is
  # the pin; we don't track Cobalt's git history). See scripts/sync.sh for the rationale.
  in_container bash -lc "
    cd /src &&
    gclient config --name src --unmanaged https://github.com/youtube/cobalt.git &&
    gclient sync -r '$pin' --no-history
  "
else
  info "Cobalt checkout already present at $COBALT_TREE — skipping initial sync."
fi

# Bootstrap the in-tree depot_tools' bundled python3 (its gn/python wrappers need
# python3_bin_reldir.txt). gclient sync fetches depot_tools as source but leaves this unbootstrapped
# with self-update off, so some build actions would fail. Idempotent; safe to re-run.
if [ ! -f "$COBALT_TREE/third_party/depot_tools/python3_bin_reldir.txt" ]; then
  info "Bootstrapping in-tree depot_tools python3 ..."
  in_container bash -c "
    cd /src/src/third_party/depot_tools &&
    DEPOT_TOOLS_UPDATE=1 bash -c 'source ./bootstrap_python3 && bootstrap_python3'
  "
fi

info "Bootstrap complete. Next: 'just gen dev' then 'just build dev'."
