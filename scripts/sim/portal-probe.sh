#!/usr/bin/env bash
# Feasibility probe for the containerized test sim: the Flatpak update portal runs and answers inside
# a plain container (no GPU, no SteamOS image). Negative control: a non-flatpak caller's
# CreateUpdateMonitor must be refused with "Updates only supported by flatpak apps".
#
# Needs Docker; runs with seccomp=unconfined so flatpak's bwrap userns is allowed.
# Env: SIM_BASE_IMAGE (default archlinux:latest).
#
# Exit: 0 portal reachable + contract enforced · 3 no Docker · 4 the portal never activated.
set -euo pipefail

command -v docker >/dev/null 2>&1 || { echo "error: docker not found" >&2; exit 3; }

image="${SIM_BASE_IMAGE:-archlinux:latest}"

# In-container: install the flatpak stack, call CreateUpdateMonitor as a non-flatpak caller.
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
