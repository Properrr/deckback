#!/usr/bin/env bash
# L0 guard for the test sim: scripts/sim/run.sh must refuse every hardware-only gate with exit 6
# (UNSUPPORTED-IN-SIM), never a pass, and must build the pinned SteamOS userspace. Needs no docker.
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

# ---- the sim must be the DEVICE's userspace, and must prove it rather than trust a tag ------------

if grep -q 'docker/steamos.Dockerfile' "$RUN"; then
  ok "run.sh builds the SteamOS-userspace image, not archlinux:latest"
else
  bad "run.sh is not building docker/steamos.Dockerfile — the sim would claim SteamOS it does not have"
fi

# incontainer.sh refuses to run unless the image declares itself SteamOS.
if grep -q 'DECKBACK_SIM_BASE' scripts/sim/incontainer.sh; then
  ok "incontainer.sh asserts the image is the SteamOS base before running a suite"
else
  bad "incontainer.sh does not verify DECKBACK_SIM_BASE — a non-SteamOS image could green the sim"
fi

# bwrap needs all three on a native Linux host (Ubuntu >= 23.10 sets
# kernel.apparmor_restrict_unprivileged_userns=1); --privileged buys nothing over this set.
for opt in 'seccomp=unconfined' 'apparmor=unconfined' 'SYS_ADMIN'; do
  if grep -q -- "$opt" "$RUN"; then
    ok "run.sh grants '$opt' (bwrap/flatpak-instance prerequisite)"
  else
    bad "run.sh does not grant '$opt' — the reconnect suite cannot run on a native Linux host"
  fi
done

# The compiler/glibc pin is what makes a sim result reproducible.
if grep -q 'DECKBACK_STEAMOS_GCC' docker/steamos.Dockerfile && grep -q 'PIN DRIFT' docker/steamos.Dockerfile; then
  ok "steamos.Dockerfile pins gcc/glibc and FAILS THE BUILD on drift"
else
  bad "steamos.Dockerfile does not assert its toolchain pin — drift would be silent"
fi

echo "sim_guardrail: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
