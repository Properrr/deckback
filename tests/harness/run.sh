#!/usr/bin/env bash
# Run every L0 harness test (tests/harness/test_*.sh). No Deck, no container, no Chromium checkout —
# these test the shell/awk logic of the harness itself, which is otherwise only exercised on hardware.
#
# Exit: 0 all passed · 1 one or more failed.
set -uo pipefail
cd "$(dirname "$0")" || exit 1

failed=0
for t in test_*.sh test_*.py; do
  [ -f "$t" ] || continue
  echo "── $t ──────────────────────────────────────────"
  case "$t" in
    *.sh) runner=(bash "$t") ;;
    # -B: never read or write .pyc. Python's bytecode cache keys on the source's mtime in whole
    # seconds plus its size, so a same-size edit inside one second silently reuses stale bytecode —
    # which is how a mutation run once reported two killed mutants as SURVIVED (finding F7).
    *.py) runner=(python3 -B "$t") ;;
  esac
  if "${runner[@]}"; then :; else
    echo "!! $t FAILED"
    failed=$((failed + 1))
  fi
  echo
done

if [ "$failed" -gt 0 ]; then
  echo "harness: ${failed} test file(s) failed"
  exit 1
fi
echo "harness: all test files passed"
