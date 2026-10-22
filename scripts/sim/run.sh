#!/usr/bin/env bash
# Containerized test sim runner: builds the sim image and runs a suite against the read-only
# bind-mounted source. GPU-independent layers only; a hardware gate is refused, never passed.
#
#   scripts/sim/run.sh [suite]   suite: all (default) | launcher | shortcut | portal | reconnect
#
# Exit (harness §1 + sim): 0 ok · 2 assert · 3 env (no docker) · 5 usage · 6 UNSUPPORTED-IN-SIM
# (a hardware-only gate was asked of the sim — it is not, and can never be, a pass here).
set -euo pipefail
cd "$(dirname "$0")/../.."

suite="${1:-all}"
image="deckback-sim-steamos:latest"

# Hardware gates cannot run in software: refuse them by name (exit 6), never a silent 0.
case "$suite" in
gpu | vaapi | decode | power | soak | resume | suspend | pixel)
  echo "sim: '$suite' is a HARDWARE gate — no GPU/battery/ACPI in a container, and lavapipe has no" >&2
  echo "     VA-API. Run it on a Deck (e.g. 'just power' / 'just soak'). Refusing to fake a pass." >&2
  exit 6
  ;;
esac

command -v docker >/dev/null 2>&1 || {
  echo "error: docker not found (env)" >&2
  exit 3
}

echo "sim: building $image ..."
docker build -q -f docker/steamos.Dockerfile -t "$image" docker >/dev/null

# bwrap (flatpak instances, the reconnect suite) needs unprivileged user namespaces; on a native Linux
# host that takes all three:
#   seccomp=unconfined    docker's default seccomp blocks the `unshare` syscall
#   apparmor=unconfined   Ubuntu >= 23.10 sets kernel.apparmor_restrict_unprivileged_userns=1
#                         (bwrap: "Failed to make / slave: Permission denied")
#   --cap-add SYS_ADMIN   otherwise bwrap: "setting up uid map: Permission denied"
# `--privileged` adds nothing over this set.
DOCKER_SANDBOX_OPTS=(
  --security-opt seccomp=unconfined
  --security-opt apparmor=unconfined
  --cap-add SYS_ADMIN
)

echo "sim: running suite '$suite' ..."
exec docker run --rm "${DOCKER_SANDBOX_OPTS[@]}" -v "$PWD:/src:ro" \
  "$image" bash /src/scripts/sim/incontainer.sh "$suite"
