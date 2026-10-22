#!/usr/bin/env bash
. "$(dirname "$0")/lib.sh"
preset="${1:-dev}"
require_cobalt_checkout
[ -d "$COBALT_TREE/out/${preset}" ] || { info "out/${preset} missing — running gen first"; ./scripts/gen.sh "$preset"; }

# `deck` defaults to -j 28 (override with NINJA_JOBS). ThinLTO/CFI are off in args/deck.gn, so -j only
# governs compile jobs. Run one build session at a time: concurrent sessions exhaust RAM.
case "$preset" in
  deck) jobs="-j ${NINJA_JOBS:-28}" ;;
  *)    jobs="${NINJA_JOBS:+-j ${NINJA_JOBS}}" ;;
esac
info "autoninja -C out/${preset} ${jobs} ${COBALT_TARGET} ..."
in_container bash -lc "cd $CTR_TREE && autoninja -C out/${preset} ${jobs} ${COBALT_TARGET}"

info "Building launcher (CMake) ..."
./scripts/launcher.sh build
info "Build complete."
