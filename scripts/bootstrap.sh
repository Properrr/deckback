#!/usr/bin/env bash
. "$(dirname "$0")/lib.sh"

info "Sanity check: shallow Cobalt checkout + one build preset wants ~50 GB free disk."
avail_gb="$(df -Pk "$REPO_ROOT" | awk 'NR==2{print int($4/1048576)}')"
[ "${avail_gb:-0}" -ge 80 ] || info "WARNING: only ${avail_gb} GB free here — Chromium may not fit."

pin="$(deps_pin)"
require_engine

# Pre-create the host bind-mount dirs (docker-compose.yml: /cache/sccache, /cache/flatpak) as the host
# user; Docker would create missing ones as root:root, unwritable by the container's uid-1000 `dev`.
mkdir -p "${SCCACHE_HOST_DIR:-./.sccache}" "${FLATPAK_HOST_DIR:-./.flatpak-cache}"

info "Building dev container image at commit $pin ..."
COBALT_COMMIT="$pin" "$CONTAINER_ENGINE" compose build dev

if [ ! -d "$COBALT_TREE/.git" ]; then
  info "gclient config + first sync into $COBALT_TREE (this is the long one) ..."
  mkdir -p "$COBALT_SRC"
  # The solution MUST be named `src` (Chrobalt's DEPS is keyed under src/). gclient runs from the root
  # (/src) so the managed dir /src/src is not itself a bind-mount point, which gclient would move to
  # _bad_scm. --no-history: shallow, single revision.
  in_container bash -lc "
    cd /src &&
    gclient config --name src --unmanaged https://github.com/youtube/cobalt.git &&
    gclient sync -r '$pin' --no-history
  "
else
  info "Cobalt checkout already present at $COBALT_TREE — skipping initial sync."
fi

# gclient sync leaves the in-tree depot_tools' bundled python3 unbootstrapped, and its gn/python
# wrappers need python3_bin_reldir.txt. Idempotent.
if [ ! -f "$COBALT_TREE/third_party/depot_tools/python3_bin_reldir.txt" ]; then
  info "Bootstrapping in-tree depot_tools python3 ..."
  in_container bash -c "
    cd /src/src/third_party/depot_tools &&
    DEPOT_TOOLS_UPDATE=1 bash -c 'source ./bootstrap_python3 && bootstrap_python3'
  "
fi

info "Bootstrap complete. Next: 'just gen dev' then 'just build dev'."
