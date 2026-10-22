#!/usr/bin/env bash
# The pre-push / pre-release gate. .githooks/pre-push and every CI job call this script, so local and
# CI run the same checks.
#
# Usage:  scripts/preflight.sh [shell|launcher|gn|all]     (default: all)
#   shell     — shellcheck + the L0 harness suite (tests/harness/run.sh)
#   launcher  — clang-format check + gcc build & ctest + clang build (-Wall -Wextra -Werror)
#   gn        — args/*.gn are overrides-only (no platform/build_type base args)
#   all       — every target above
#
# Needs no Chromium checkout, Deck or Docker. Pinned lint tools come from lib.sh; the launcher build
# needs cmake, ninja, g++, clang and the dev libs named in the die_env messages.
#
# Exit: 0 green · 2 a check failed · 3 a required tool/lib is missing · 5 usage.
. "$(dirname "$0")/lib.sh"

target="${1:-all}"
case "$target" in shell | launcher | gn | all) : ;; *) die_usage "usage: preflight.sh [shell|launcher|gn|all]" ;; esac

# ---- gn: args/*.gn are overrides only -----------------------------------------------------------
run_gn() {
  local bad=0 f
  for f in args/*.gn; do
    [ -e "$f" ] || continue
    if grep -qE '^\s*(target_os|target_cpu|is_debug|is_official_build)\b' "$f"; then
      echo "  FAIL  $f sets a platform/build_type base arg; args/*.gn must contain only overrides" >&2
      bad=1
    fi
  done
  return "$bad"
}

# ---- shell: shellcheck + the L0 harness suite ---------------------------------------------------
run_shell() {
  local bad=0 sc
  sc="$(pinned_tool shellcheck shellcheck shellcheck-py)"
  "$sc" -S warning scripts/*.sh scripts/sim/*.sh tests/harness/*.sh || bad=1
  # The harness suite includes test_cdp_lib.py / test_video_corruption.py, which import Pillow. A
  # missing Pillow ERRORs the run — surface it as an environment gap, not a mystery test failure.
  python3 -c 'import PIL' 2>/dev/null || die_env "the harness suite needs Pillow (CI: python3-pil). Install it: pip install Pillow"
  ./tests/harness/run.sh || bad=1
  # A manifest that adds a sandbox permission strands every installed user on their current version
  # — flatpak-portal refuses the self-update outright (findings/durable/self-update.md). Cheap to
  # check, invisible until a user reports "it says it updated but it didn't", so it runs every push.
  ./scripts/check-permissions.sh || bad=1
  return "$bad"
}

# ---- launcher: format check + gcc build & ctest + clang build -----------------------------------
run_launcher() {
  local bad=0 cf
  command -v cmake >/dev/null && command -v ninja >/dev/null ||
    die_env "the launcher build needs cmake + ninja. Install: cmake ninja-build (and g++, clang, libsystemd-dev libcurl4-openssl-dev libpulse-dev libxcb1-dev for a CI-equivalent build)"

  # 1. clang-format check (`just fmt` is the fixer).
  cf="$(clang_format_bin)"
  local files
  mapfile -t files < <(find launcher/src launcher/tests \( -name '*.cpp' -o -name '*.hpp' \))
  if [ "${#files[@]}" -gt 0 ] && ! "$cf" --style=file --dry-run --Werror "${files[@]}" 2>/dev/null; then
    echo "  FAIL  launcher/ is not clang-format $CLANG_FORMAT_VERSION clean — run 'just fmt', commit, retry:" >&2
    "$cf" --style=file --dry-run --Werror "${files[@]}" 2>&1 | grep -oE 'launcher/[^:]+\.(cpp|hpp)' | sort -u | sed 's/^/          /' >&2
    bad=1
  fi

  # 2. gcc: Release build + ctest.  3. clang: build. Built in throwaway dirs so the push never dirties
  # the tree (the whole point of a pre-push gate is to leave the tree exactly as the user staged it).
  local gdir cdir
  gdir="$(mktemp -d "${TMPDIR:-/tmp}/deckback-preflight-gcc.XXXXXX")"
  cdir="$(mktemp -d "${TMPDIR:-/tmp}/deckback-preflight-clang.XXXXXX")"
  # shellcheck disable=SC2064
  trap "rm -rf '$gdir' '$cdir'" RETURN

  if cmake -S launcher -B "$gdir" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 &&
    cmake --build "$gdir" >/dev/null 2>&1; then
    ctest --test-dir "$gdir" --output-on-failure >/dev/null 2>&1 || {
      echo "  FAIL  launcher ctest (gcc) failed — rerun: ctest --test-dir <build> --output-on-failure" >&2
      bad=1
    }
  else
    echo "  FAIL  launcher gcc build failed" >&2
    bad=1
  fi

  if command -v clang++ >/dev/null 2>&1; then
    CXX=clang++ cmake -S launcher -B "$cdir" -G Ninja >/dev/null 2>&1 &&
      cmake --build "$cdir" >/dev/null 2>&1 ||
      {
        echo "  FAIL  launcher clang build failed (clang -Wall -Wextra -Werror is stricter than gcc)" >&2
        bad=1
      }
  else
    die_env "the launcher clang build needs clang++ (CI runs both gcc and clang). Install: clang"
  fi
  return "$bad"
}

# ---- dispatch -----------------------------------------------------------------------------------
rc=0
run_one() {
  info "preflight: $1"
  "run_$1" || {
    rc="$EX_ASSERT"
    echo "  ✗ $1 FAILED" >&2
    return
  }
  echo "  ✓ $1 clean" >&2
}
case "$target" in
gn) run_one gn ;;
shell) run_one shell ;;
launcher) run_one launcher ;;
all)
  run_one gn
  run_one shell
  run_one launcher
  ;;
esac

if [ "$rc" -eq 0 ]; then
  info "preflight: ALL GREEN ✓ (mirrors .github/workflows/lint.yml)"
else
  echo "error: preflight FAILED — the tree would land red in CI. Fix the above, then retry." >&2
fi
exit "$rc"
