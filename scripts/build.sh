#!/usr/bin/env bash
. "$(dirname "$0")/lib.sh"
preset="${1:-dev}"
require_cobalt_checkout
[ -d "$COBALT_TREE/out/${preset}" ] || { info "out/${preset} missing — running gen first"; ./scripts/gen.sh "$preset"; }

# Compile concurrency for the `deck` build. Two earlier OOMs, both now understood: (1) ThinLTO/CFI
# link codegen peaked ~30-60 GB — fixed by forcing them OFF in args/deck.gn + concurrent_links=1;
# (2) the later -j24->12 drop was a MISDIAGNOSIS — the SIGKILL came from *multiple concurrent build
# sessions* on this swapless 62 GB host (two -j24 runs = ~48 jobs racing for RAM on the same out/),
# not from -j24 itself. A single session is the invariant now, so -j24 was never the problem.
# With LTO gone, `-j` only governs compile jobs (~few hundred MB each; -j12 peaked ~10 GB of 62), so
# use most of the 5950X's 32 threads. 28 leaves headroom for the serialized link; override NINJA_JOBS.
case "$preset" in
  deck) jobs="-j ${NINJA_JOBS:-28}" ;;
  *)    jobs="${NINJA_JOBS:+-j ${NINJA_JOBS}}" ;;
esac
info "autoninja -C out/${preset} ${jobs} ${COBALT_TARGET} ..."
in_container bash -lc "cd $CTR_TREE && autoninja -C out/${preset} ${jobs} ${COBALT_TARGET}"

info "Building launcher (CMake) ..."
./scripts/launcher.sh build
info "Build complete."
