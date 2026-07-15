#!/usr/bin/env bash
# scripts/preflight.sh — the SINGLE source of truth for the "before it leaves this machine" gate.
#
# Both .githooks/pre-push AND .github/workflows/lint.yml call this exact script, so a local push and
# a CI run execute the same checks by construction — they cannot drift. That drift is precisely what
# let a stale test, unformatted C++, and a clang-only -Werror all land red in one afternoon: the old
# pre-push hook mirrored only *part* of CI (clang-format + shellcheck), so the harness tests and the
# launcher builds — the two gates that actually caught those bugs — never ran locally.
#
# Usage:  scripts/preflight.sh [shell|launcher|gn|all]     (default: all)
#   shell     — shellcheck + the L0 harness suite (tests/harness/run.sh)
#   launcher  — clang-format-18 check + gcc build & ctest + clang build (-Wall -Wextra -Werror)
#   gn        — args/*.gn are overrides-only (no platform/build_type base args)
#   all       — every target above, in CI's job order
#
# Needs NO Chromium checkout, NO Deck, NO Docker — it mirrors CI's free-runner jobs, so it runs on any
# contributor's machine. Pinned lint tools (clang-format-18, shellcheck) are bootstrapped into a
# cached venv when the system lacks them; the launcher build needs a real toolchain + dev libs (see
# the die_env messages / .internal/HARNESS.md).
#
# Exit: 0 all green · 2 a check FAILED (the tree is wrong — fix before pushing) · 3 environment
# (a required tool/lib is missing) · 5 usage.
. "$(dirname "$0")/lib.sh"

target="${1:-all}"
case "$target" in shell | launcher | gn | all) : ;; *) die_usage "usage: preflight.sh [shell|launcher|gn|all]" ;; esac

# The pinned-tool bootstrap (pinned_tool + venv) lives in lib.sh, shared with fmt.sh so `just fmt`
# writes the same clang-format-18 style this gate checks.

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
  "$sc" -S warning scripts/*.sh tests/harness/*.sh || bad=1
  # The harness suite includes test_cdp_lib.py / test_video_corruption.py, which import Pillow. A
  # missing Pillow ERRORs the run — surface it as an environment gap, not a mystery test failure.
  python3 -c 'import PIL' 2>/dev/null || die_env "the harness suite needs Pillow (CI: python3-pil). Install it: pip install Pillow"
  ./tests/harness/run.sh || bad=1
  return "$bad"
}

# ---- launcher: format check + gcc build & ctest + clang build -----------------------------------
run_launcher() {
  local bad=0 cf
  command -v cmake >/dev/null && command -v ninja >/dev/null ||
    die_env "the launcher build needs cmake + ninja. Install: cmake ninja-build (and g++, clang, libsystemd-dev libcurl4-openssl-dev libpulse-dev libxcb1-dev for a CI-equivalent build)"

  # 1. clang-format-18 — check only (CI can't commit; `just fmt` is the fixer).
  cf="$(pinned_tool clang-format-18 clang-format clang-format==18.1.8)"
  local files
  mapfile -t files < <(find launcher/src launcher/tests \( -name '*.cpp' -o -name '*.hpp' \))
  if [ "${#files[@]}" -gt 0 ] && ! "$cf" --style=file --dry-run --Werror "${files[@]}" 2>/dev/null; then
    echo "  FAIL  launcher/ is not clang-format-18 clean — run 'just fmt', commit, retry:" >&2
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
