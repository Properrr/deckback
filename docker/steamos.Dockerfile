# Runtime image for the containerized test sim — a REAL SteamOS userspace, not "Arch, close enough".
# (findings/durable/test-sim.md)
#
# NOT a build image for Chromium/Cobalt (that stays Debian-12) and NOT a device simulator: it runs the
# GPU-INDEPENDENT layers only — the launcher's own build+L0, the Flatpak update portal (self-update /
# D-Bus reconnect), and the installer's Steam-shortcut/artwork writing. It is deliberately incapable of
# exercising VA-API decode, power, or suspend/resume; the runner refuses to green those (exit 6).
#
# ---- why this replaced `FROM archlinux:latest` ---------------------------------------------------
#
# The old sim base was archlinux:latest, under a comment reading "Arch base — honouring 'SteamOS is
# Arch-based'". That is true and it is not enough. archlinux:latest is a ROLLING release; SteamOS 3.x
# is a FROZEN Arch snapshot plus Valve's holo/jupiter packages. Measured 2026-07-16, Deck (SteamOS
# 3.8.15, Galileo/OLED) vs archlinux:latest that same day:
#
#     package        Deck                    archlinux:latest
#     glibc          2.41                    2.43
#     gcc-libs       15.1.1                  16.1.1     <-- a full major ahead
#     systemd-libs   257.7                   261.1      <-- sd-bus is the updater's transport
#     curl           8.15.0                  8.21.0
#     flatpak        1.16.6                  1.18.0     <-- the portal IS the self-update foundation
#     libevdev       1.13.4                  1.13.6
#     libxcb         1.17.0-1                1.17.0-1   (the only exact match)
#
# So `ok - launcher builds + L0 green on Arch` was an honest sentence answering a DIFFERENT question
# than a reader takes from it: it proved the launcher builds against tomorrow's glibc/gcc/libsystemd,
# not against the Deck's. It compiled with GCC 16 — which is neither of the two toolchains the project
# pins (Clang >=18 / GCC 14) NOR the one SteamOS ships. Same family as `just power` reporting
# `mean 0.00 W ... PASS` off a missing battery node: a check aimed slightly past its target.
#
# ---- the repos are READ OFF THE DEVICE, never guessed ---------------------------------------------
#
# The branch below is `3.8.1x`, taken from the Deck's own /etc/pacman.conf via
# scripts/sim/capture-deck-baseline.sh -> docker/steamos-baseline.env. Guessing would have produced
# "holo-3.5" and been silently wrong. Re-run the capture after a SteamOS update and bump
# DECKBACK_STEAMOS_BRANCH to match; the diff of steamos-baseline.env is the upgrade note.
#
# Verified 2026-07-16: every package this image installs resolves to the exact version string the
# Deck reports — glibc 2.41+r65+ge7c419a29575-1, gcc 15.1.1+r7+gf36ec88aa85a-1, systemd-libs
# 257.7-2.5, curl 8.15.0-1, libpulse 17.0+r43+g3e2bb8a1e-1, libxcb 1.17.0-1, libevdev 1.13.4-1,
# flatpak 1:1.16.6-1.1, bubblewrap 0.11.0-1.

# Stage 1: bootstrap a pure SteamOS rootfs. archlinux is used ONLY to run pacman; nothing from it is
# kept, so no rolling-release package can leak into the result.
FROM archlinux:latest AS rootfs

ARG DECKBACK_STEAMOS_BRANCH=3.8.1x
ARG DECKBACK_STEAMOS_MIRROR=https://steamdeck-packages.steamos.cloud/archlinux-mirror

# SigLevel=Never: Valve signs holo/jupiter with their own key, which archlinux-keyring does not carry,
# and importing it here would be its own trust decision. The fetch is HTTPS direct from Valve's host,
# and this image is a local test sim that never ships. If it ever gates a release, revisit this.
RUN set -eux; \
    printf 'Server = %s/$repo/os/$arch\n' "${DECKBACK_STEAMOS_MIRROR}" > /etc/pacman.d/steamos-mirrorlist; \
    { \
      echo '[options]'; \
      echo 'Architecture = auto'; \
      echo 'SigLevel = Never'; \
      echo 'ParallelDownloads = 10'; \
      for r in jupiter holo core extra multilib; do \
        echo "[${r}-${DECKBACK_STEAMOS_BRANCH}]"; \
        echo 'Include = /etc/pacman.d/steamos-mirrorlist'; \
      done; \
    } > /etc/pacman-steamos.conf

# One layer: the launcher's out-of-tree build deps (C++23 + libsystemd/libcurl/libpulse/xcb/evdev),
# the Flatpak-portal stack (flatpak + its portal, ostree, dbus, gdbus via glib2, bubblewrap), and
# python for steam_shortcuts.py + the assert helpers. Mirrors what docker/sim.Dockerfile installed,
# so the only variable that changed is WHERE the packages come from.
RUN set -eux; \
    mkdir -p /rootfs/var/lib/pacman /rootfs/etc; \
    pacman --config /etc/pacman-steamos.conf --root /rootfs --dbpath /rootfs/var/lib/pacman \
      -Sy --noconfirm --needed \
      base bash coreutils gawk grep sed procps-ng findutils \
      gcc cmake ninja pkgconf make \
      systemd-libs curl libpulse libxcb libevdev \
      flatpak ostree dbus glib2 bubblewrap python; \
    cp /etc/pacman.d/steamos-mirrorlist /rootfs/etc/pacman.d/steamos-mirrorlist 2>/dev/null || true; \
    rm -rf /rootfs/var/cache/pacman/pkg/*

# ---- the pin -------------------------------------------------------------------------------------
#
# The point of the pin is REPRODUCIBILITY, and it is worth being exact about what it does and does not
# mean. The Deck never compiles anything: the shipped launcher is built by the FLATPAK SDK's GCC
# (org.freedesktop.Platform//25.08, GCC 14 — doc §13.2's "dual-clean on Clang >=18 and GCC 14"), and
# runs against that runtime's libs, not SteamOS's. So this compiler is not the release toolchain and
# must never be described as one.
#
# What it buys: archlinux:latest silently swapped the compiler under the sim (GCC 16 the day this was
# written, something else next month), so a green sim run was not reproducible and its toolchain was
# nobody's — not the SDK's, not the device's. Pinning to the version SteamOS 3.8.15 actually carries
# makes the sim's answer stable, makes drift LOUD instead of silent, and adds a third real-world
# compiler to the C++23 -Werror surface.
#
# These values come from the Deck via scripts/sim/capture-deck-baseline.sh (PKG_GCC_LIBS / PKG_GLIBC
# in docker/steamos-baseline.env), not from memory. Valve can move them within a branch, so ASSERT
# rather than assume: a silent bump is the exact failure this image exists to stop, and an assertion
# that cannot fail is not an assertion (HARNESS §0).
ARG DECKBACK_STEAMOS_GCC=15.1.1+r7+gf36ec88aa85a-1
ARG DECKBACK_STEAMOS_GLIBC=2.41+r65+ge7c419a29575-1
RUN set -eux; \
    q() { pacman --root /rootfs --dbpath /rootfs/var/lib/pacman -Q "$1" | awk '{print $2}'; }; \
    got_gcc="$(q gcc)"; got_glibc="$(q glibc)"; \
    if [ "$got_gcc" != "${DECKBACK_STEAMOS_GCC}" ]; then \
      echo "PIN DRIFT: gcc is '$got_gcc', pinned to '${DECKBACK_STEAMOS_GCC}'." >&2; \
      echo "  Valve moved the compiler inside this branch. Re-run scripts/sim/capture-deck-baseline.sh" >&2; \
      echo "  against a Deck, bump the ARG to match, and re-run 'just sim all' before trusting a green." >&2; \
      exit 1; \
    fi; \
    if [ "$got_glibc" != "${DECKBACK_STEAMOS_GLIBC}" ]; then \
      echo "PIN DRIFT: glibc is '$got_glibc', pinned to '${DECKBACK_STEAMOS_GLIBC}'." >&2; \
      exit 1; \
    fi; \
    echo "pinned: gcc=$got_gcc glibc=$got_glibc"

# Stage 2: the SteamOS rootfs alone. Nothing from archlinux:latest survives this line.
FROM scratch
COPY --from=rootfs /rootfs/ /

# Recorded so a running container can prove what it is (and so the sim runner can assert it, rather
# than trusting the image tag).
ARG DECKBACK_STEAMOS_BRANCH=3.8.1x
ARG DECKBACK_STEAMOS_GCC=15.1.1+r7+gf36ec88aa85a-1
ENV DECKBACK_SIM_BASE=steamos \
    DECKBACK_SIM_BRANCH=${DECKBACK_STEAMOS_BRANCH} \
    DECKBACK_SIM_GCC=${DECKBACK_STEAMOS_GCC}

# Explicit, so CMake cannot silently pick something else up if this image ever grows a second
# compiler. The launcher is C++23 -Wall -Wextra -Werror on GCC here.
ENV CC=gcc \
    CXX=g++

# Flatpak needs unprivileged user namespaces for bwrap. On a WSL2 host plain
# `--security-opt seccomp=unconfined` sufficed, which is why the reconnect suite was recorded green;
# on a native Linux host with AppArmor's unprivileged-userns restriction (Ubuntu >= 23.10) that is NOT
# enough and bwrap dies with "Failed to make / slave: Permission denied". scripts/sim/run.sh passes
# the full set. Source is bind-mounted at /src, never baked in (same rule as the dev image).
WORKDIR /src
