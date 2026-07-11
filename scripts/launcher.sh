#!/usr/bin/env bash
# Standalone launcher build+test — deliberately needs NO Chromium checkout or container (doc §13.2).
. "$(dirname "$0")/lib.sh"
cmd="${1:-build}"
cfg_flags=()
[ "${2:-}" = "asan" ] && cfg_flags+=(-DDECKBACK_ASAN=ON)

cmake -S launcher -B launcher/build -G Ninja -DCMAKE_BUILD_TYPE=Release "${cfg_flags[@]}"
cmake --build launcher/build

case "$cmd" in
  build) : ;;
  test)  ctest --test-dir launcher/build --output-on-failure ;;
  *)     die "usage: launcher.sh [build|test] [asan]" ;;
esac
