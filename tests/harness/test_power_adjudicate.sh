#!/usr/bin/env bash
# L0 tests for scripts/lib/power_adjudicate.awk — the P4 power gate's adjudication rules.
#
# Why this file exists: `just power` once printed `mean 0.00 W over 300 samples` and exited 0 on a
# Deck with no readable battery node, because awk coerces "" to 0. The P4 gate is "<= 9 W", so a run
# that measured NOTHING scored a perfect pass. These tests pin the rules that make that impossible.
#
# Needs no Deck, no container, no Chromium. Run: tests/harness/test_power_adjudicate.sh
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1

AWK_PROG="scripts/lib/power_adjudicate.awk"
[ -f "$AWK_PROG" ] || { echo "error: missing $AWK_PROG" >&2; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
pass=0 fail=0

# adjudicate <max-watts> <csv-body...>  -> echoes exit code, stderr captured to $tmp/err
adjudicate() {
  local max="$1"; shift
  local csv="$tmp/in.csv"
  printf 'sample,watts\n' > "$csv"
  local line
  for line in "$@"; do printf '%s\n' "$line" >> "$csv"; done
  awk -F, -v max="$max" -f "$AWK_PROG" "$csv" 2> "$tmp/err"
  echo $?
}

expect() {
  local name="$1" want_code="$2" got_code="$3" want_text="${4:-}"
  if [ "$got_code" != "$want_code" ]; then
    echo "FAIL $name: exit $got_code, want $want_code"; echo "  stderr: $(cat "$tmp/err")"
    fail=$((fail + 1)); return
  fi
  if [ -n "$want_text" ] && ! grep -qF "$want_text" "$tmp/err"; then
    echo "FAIL $name: stderr lacks '$want_text'"; echo "  stderr: $(cat "$tmp/err")"
    fail=$((fail + 1)); return
  fi
  echo "ok   $name"
  pass=$((pass + 1))
}

# --- THE REGRESSION THIS FILE EXISTS FOR --------------------------------------------------------
# Empty watts fields are missing data, NOT zero watts. They must void the run, never average to a
# perfect score. If this test ever passes with exit 0, the gate has gone blind again.
expect "empty samples void the run (never 'mean 0.00 W, PASS')" \
  3 "$(adjudicate 9 "1," "2," "3,")" "measurement void"

expect "a single bad sample among good ones voids the run" \
  3 "$(adjudicate 9 "1,5.00" "2," "3,5.00")" "1/3 samples unreadable"

expect "non-numeric garbage voids the run" \
  3 "$(adjudicate 9 "1,abc" "2,5.00")" "measurement void"

expect "no samples at all is void, not a pass" \
  3 "$(adjudicate 9)" "no samples recorded"

# --- the ordinary gate ---------------------------------------------------------------------------
expect "mean under the budget passes" \
  0 "$(adjudicate 9 "1,4.00" "2,6.00")" "PASS"

expect "mean over the budget fails with the assertion code" \
  2 "$(adjudicate 9 "1,12.00" "2,14.00")" "FAIL: mean draw exceeds the gate"

# Exactly at the budget passes: the gate is "<= 9 W", not "< 9 W".
expect "mean exactly at the budget passes" \
  0 "$(adjudicate 9 "1,9.00" "2,9.00")" "PASS"

expect "a hair over the budget fails" \
  2 "$(adjudicate 9 "1,9.00" "2,9.02")" "FAIL"

expect "the budget is configurable" \
  0 "$(adjudicate 15 "1,12.00" "2,14.00")" "PASS"

# The reported sample count must be the number actually averaged, so a human reading the line can
# sanity-check it against the requested duration.
expect "reports the sample count it averaged" \
  0 "$(adjudicate 9 "1,1.00" "2,2.00" "3,3.00")" "over 3 samples"

echo
echo "power_adjudicate: ${pass} passed, ${fail} failed"
[ "$fail" -eq 0 ]
