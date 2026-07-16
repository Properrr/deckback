#!/usr/bin/env bash
# Containerized test sim runner (findings/durable/test-sim.md). Builds the sim image and runs a suite
# inside it against the bind-mounted (read-only) source. Runs the GPU-INDEPENDENT layers only and, by
# construction, REFUSES to run — let alone green — a hardware gate.
#
#   scripts/sim/run.sh [suite]   suite: all (default) | launcher | shortcut | portal
#
# Exit (harness §1 + sim): 0 ok · 2 assert · 3 env (no docker) · 5 usage · 6 UNSUPPORTED-IN-SIM
# (a hardware-only gate was asked of the sim — it is not, and can never be, a pass here).
set -euo pipefail
cd "$(dirname "$0")/../.."

suite="${1:-all}"
image="deckback-sim:latest"

# The guardrail, first and loud: the gates that actually gate a release can't run in software, and a
# sim that faked them would be the m114 false-pass trap. Name them explicitly so `just sim vaapi` is a
# clear refusal (exit 6), never a silent 0.
case "$suite" in
gpu | vaapi | decode | power | soak | resume | suspend | pixel)
  echo "sim: '$suite' is a HARDWARE gate — no GPU/battery/ACPI in a container, and lavapipe has no" >&2
  echo "     VA-API. Run it on a Deck (e.g. 'just power' / 'just soak'). Refusing to fake a pass." >&2
  exit 6
  ;;
esac

command -v docker >/dev/null 2>&1 || { echo "error: docker not found (env)" >&2; exit 3; }

echo "sim: building $image ..."
docker build -q -f docker/sim.Dockerfile -t "$image" docker >/dev/null

echo "sim: running suite '$suite' ..."
exec docker run --rm --security-opt seccomp=unconfined -v "$PWD:/src:ro" \
  "$image" bash /src/scripts/sim/incontainer.sh "$suite"
