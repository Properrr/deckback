#!/usr/bin/env bash
# L0 guard for the test sim's honesty rule (findings/durable/test-sim.md): scripts/sim/run.sh MUST
# refuse every hardware-only gate with exit 6 (UNSUPPORTED-IN-SIM), never a pass. A software sim that
# could green a GPU / VA-API / power / resume gate would be the m114 false-pass trap — this test is
# what keeps that structurally impossible, rather than relying on developer discipline.
#
# The refusal happens before any docker use, so this needs no docker and no container.
# Run: tests/harness/test_sim_guardrail.sh
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1

RUN=scripts/sim/run.sh
[ -x "$RUN" ] || { echo "error: missing $RUN" >&2; exit 1; }

pass=0 fail=0
ok() { echo "ok   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1"; fail=$((fail + 1)); }

# Every gate that needs the Deck's GPU/battery/ACPI must be refused, not faked.
for s in gpu vaapi decode power soak resume suspend pixel; do
  "$RUN" "$s" >/dev/null 2>&1
  rc=$?
  if [ "$rc" -eq 6 ]; then ok "refuses hardware gate '$s' (exit 6 UNSUPPORTED-IN-SIM)"; else bad "'$s': exit $rc, want 6"; fi
done

echo "sim_guardrail: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
