#!/usr/bin/env bash
# L0 tests for the pieces of `just power` that decide WHAT gets measured, as opposed to
# power_adjudicate.awk, which decides whether what was measured passes.
#
#   scripts/lib/power_watts.awk   raw sysfs microunits -> watts
#   ac_is_online   (lib.sh)       is the Deck on the charger?
#   panel_is_dark  (lib.sh)       is the screen off?
#
# All three exist because a wrong answer from any of them produces a *plausible* number under the
# 9 W budget rather than an obvious zero:
#
#   * On AC, `current_now` is the charging current: P = V x I measures energy going INTO the battery
#     (~4.6 W on the OLED). Under budget. Meaningless.
#   * With the panel off, real draw drops by watts on a device whose whole gate is "<= ~9 W".
#   * With no `power_now` node -- which is every OLED Deck -- the old glob matched nothing and the
#     gate exited 3 forever, so it never ran on the only hardware this project has.
#
# Needs no Deck, no container, no Chromium. Run: tests/harness/test_power_probe.sh
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1

AWK_PROG="scripts/lib/power_watts.awk"
[ -f "$AWK_PROG" ] || { echo "error: missing $AWK_PROG" >&2; exit 1; }
# lib.sh is the unit under test here, but sourcing it also imports its `set -euo pipefail`, which
# would abort this file at the first deliberately-failing assertion. Undo that, not the sourcing.
# shellcheck disable=SC1091
. ./scripts/lib.sh
set +e +o pipefail

pass=0 fail=0
ok()  { pass=$((pass + 1)); echo "  ok   - $1"; }
bad() { fail=$((fail + 1)); echo "  FAIL - $1"; }

eq() { # eq <desc> <want> <got>
  if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (want '$2', got '$3')"; fi
}

# watts <method> <csv-lines...>  -> stdout of the converter
watts() {
  local method="$1"; shift
  printf '%s\n' "$@" | awk -F, -v method="$method" -f "$AWK_PROG"
}

echo "── power_watts.awk: power_now (microwatts) ──"
eq "9 W reads as 9.00"            "1,9.00"  "$(watts power_now '1,9000000,')"
eq "fractional watts survive"     "1,4.94"  "$(watts power_now '1,4935284,')"

echo "── power_watts.awk: V x I (the OLED path) ──"
# 8.171 V x 0.604 A = 4.935 W. These are literal readings taken from the OLED unit.
eq "voltage x current -> watts"   "1,4.94"  "$(watts vi '1,8171000,604000')"
eq "a 9 W draw at 7.8 V"          "1,9.05"  "$(watts vi '1,7800000,1160000')"

echo "── an unusable reading is EMPTY, never zero ──"
# This is the whole point. power_adjudicate.awk voids a run containing an empty field; it would
# average a literal 0 straight through the "<= 9 W" gate.
eq "missing power_now -> empty"   "1,"      "$(watts power_now '1,,')"
eq "missing current -> empty"     "1,"      "$(watts vi '1,8171000,')"
eq "missing voltage -> empty"     "1,"      "$(watts vi '1,,604000')"
eq "zero current -> empty"        "1,"      "$(watts vi '1,8171000,0')"
eq "zero power_now -> empty"      "1,"      "$(watts power_now '1,0,')"
eq "non-numeric -> empty"         "1,"      "$(watts power_now '1,garbage,')"
eq "negative -> empty"            "1,"      "$(watts vi '1,8171000,-604000')"

echo "── the converter never silently invents a method ──"
awk -F, -v method=bogus -f "$AWK_PROG" >/dev/null 2>&1 <<<'1,1,1'
rc=$?
[ "$rc" -eq 3 ] && ok "unknown method exits 3 (ENV)" || bad "unknown method exited $rc, want 3"

echo "── power_now and V x I agree on the same battery ──"
# Same physical draw expressed both ways must grade identically, or the OLED path silently
# reports a different number from the LCD path and the two units can never be compared.
a="$(watts power_now '1,4935284,')"
b="$(watts vi '1,8171000,604000')"
eq "both methods give the same watts" "$a" "$b"

echo "── ac_is_online ──"
ac_is_online "1"        && ok "'1' is online"           || bad "'1' should be online"
ac_is_online "0"        && bad "'0' must not be online" || ok "'0' is offline"
ac_is_online ""         && bad "'' must not be online"  || ok "empty is offline (no AC node)"
# Several supplies, one plugged in: `cat AC*/online ADP*/online` yields a line each.
ac_is_online "$(printf '0\n1\n')" && ok "any online supply counts" || bad "0+1 should be online"
ac_is_online "$(printf '0\n0\n')" && bad "0+0 must not be online"  || ok "0+0 is offline"

echo "── panel_is_dark ──"
panel_is_dark "Off" "1207"   && ok "dpms Off is dark even at full brightness" || bad "dpms Off should be dark"
panel_is_dark "On"  "0"      && ok "brightness 0 is dark"                     || bad "brightness 0 should be dark"
panel_is_dark "On"  "1207"   && bad "a lit panel must not read as dark"       || ok "On + brightness is lit"
# An unreadable brightness is not evidence of darkness. Guessing "dark" here would block every run
# on a Deck whose backlight node moved; guessing "lit" is the caller's explicit choice.
panel_is_dark "On"  "unknown" && bad "unknown brightness must not read as dark" || ok "unknown brightness is not proof of darkness"
panel_is_dark "unknown" ""    && bad "unknown dpms must not read as dark"       || ok "unknown dpms is not proof of darkness"

echo
echo "test_power_probe: ${pass} passed, ${fail} failed"
[ "$fail" -eq 0 ] || exit 1
