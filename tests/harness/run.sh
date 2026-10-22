#!/usr/bin/env bash
# Run every L0 harness test (tests/harness/test_*.{sh,py}); needs no Deck, container or checkout.
# Exit: 0 all passed · 1 one or more failed.
set -uo pipefail
cd "$(dirname "$0")" || exit 1

failed=0
for t in test_*.sh test_*.py; do
  [ -f "$t" ] || continue
  echo "── $t ──────────────────────────────────────────"
  case "$t" in
    *.sh) runner=(bash "$t") ;;
    # -B: no .pyc — the cache keys on whole-second mtime + size, so a quick same-size edit can
    # reuse stale bytecode.
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
