# Runtime image for the containerized test sim (findings/durable/test-sim.md).
#
# NOT a build image for Chromium/Cobalt (that stays Debian-12) and NOT a device simulator: it runs the
# GPU-INDEPENDENT layers only — the launcher's own build+L0, the Flatpak update portal (self-update /
# D-Bus reconnect), and the installer's Steam-shortcut/artwork writing. It is deliberately incapable of
# exercising VA-API decode, power, or suspend/resume; the runner refuses to green those (exit 6).
#
# Arch base — honouring "SteamOS is Arch-based" — though the portal/flatpak layer is distro-agnostic.
FROM archlinux:latest

# One layer: the launcher's out-of-tree build deps (C++23 + libsystemd/libcurl/libpulse/xcb/evdev),
# the Flatpak-portal stack (flatpak + its portal, ostree, dbus, gdbus via glib2, bubblewrap), and
# python for steam_shortcuts.py + the assert helpers.
RUN pacman -Syu --noconfirm --needed \
      gcc cmake ninja pkgconf make \
      systemd-libs curl libpulse libxcb libevdev \
      flatpak ostree dbus glib2 bubblewrap python \
    && pacman -Scc --noconfirm

# Flatpak needs unprivileged user namespaces for bwrap — run the container with
# `--security-opt seccomp=unconfined` (scripts/sim/run.sh does). Source is bind-mounted at /src, never
# baked in (same rule as the dev image).
WORKDIR /src
