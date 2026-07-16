#!/usr/bin/env bash
# Phase-1 feasibility probe for the containerized test sim (findings/durable/test-sim.md).
#
# Proves, reproducibly, that the Flatpak update portal — the thing the self-updater + D-Bus reconnect
# talk to — runs and answers inside a plain container, with NO GPU and NO SteamOS image. It asserts
# the portal's caller contract as a negative control: a non-flatpak caller's CreateUpdateMonitor must
# be refused with "Updates only supported by flatpak apps". That exact refusal proves the portal (and,
# in the log, the PermissionStore the consent seed uses) activated and is enforcing its contract — so
# the real harness just needs a flatpak-instance caller (`flatpak run` of a minimal app).
#
# This is groundwork, not the final `just sim` harness. Needs Docker; runs an Arch container with
# --security-opt seccomp=unconfined (so flatpak's bwrap userns is allowed).
#
# Exit: 0 portal reachable + contract enforced · 3 no Docker · 4 the portal never activated.
set -euo pipefail

command -v docker >/dev/null 2>&1 || { echo "error: docker not found" >&2; exit 3; }

image="${SIM_BASE_IMAGE:-archlinux:latest}"

# In-container probe: install the flatpak stack, then call CreateUpdateMonitor from a non-flatpak
# caller over a fresh session bus and print the portal's reply.
read -r -d '' probe <<'INCONTAINER' || true
set -e
pacman -Sy --noconfirm --needed flatpak glib2 >/tmp/pac.log 2>&1 || { tail -5 /tmp/pac.log; exit 3; }
echo "flatpak: $(flatpak --version)"
dbus-run-session -- gdbus call --session --dest org.freedesktop.portal.Flatpak \
  --object-path /org/freedesktop/portal/Flatpak \
  --method org.freedesktop.portal.Flatpak.CreateUpdateMonitor "{}" 2>&1 || true
INCONTAINER

out="$(docker run --rm --security-opt seccomp=unconfined "$image" bash -c "$probe" 2>&1)"
echo "$out"

if printf '%s' "$out" | grep -q "Updates only supported by flatpak apps"; then
  echo ">>> PASS: flatpak-portal activated and enforced its caller contract in-container (Phase-1 foundation feasible)."
  exit 0
fi
echo ">>> FAIL: the portal did not activate / answer as expected in-container." >&2
exit 4
