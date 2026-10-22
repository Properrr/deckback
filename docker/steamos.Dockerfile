# Runtime image for the containerized test sim: a real SteamOS userspace (findings/durable/test-sim.md).
# Not a Chromium build image and not a device simulator: it runs only the GPU-independent layers (the
# launcher build + L0, the Flatpak update portal, the installer's Steam-shortcut writing); the runner
# refuses VA-API/power/suspend gates (exit 6).
#
# The repo branch and the gcc/glibc pins are read off the Deck by scripts/sim/capture-deck-baseline.sh
# (docker/steamos-baseline.env); after a SteamOS update, re-run the capture and bump them to match.

# Stage 1: bootstrap a pure SteamOS rootfs. archlinux is used ONLY to run pacman; nothing from it is
# kept.
FROM archlinux:latest AS rootfs

ARG DECKBACK_STEAMOS_BRANCH=3.8.1x
ARG DECKBACK_STEAMOS_MIRROR=https://steamdeck-packages.steamos.cloud/archlinux-mirror

# SigLevel=Never: Valve's holo/jupiter signing key is not in archlinux-keyring. Acceptable only for a
# local test image fetched over HTTPS from Valve's host; revisit if it ever gates a release.
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

# The launcher's build deps (C++23 + libsystemd/libcurl/libpulse/xcb/evdev), the Flatpak-portal stack
# (flatpak, ostree, dbus, gdbus via glib2, bubblewrap), and python for steam_shortcuts.py + helpers.
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
# gcc/glibc pinned to the Deck's versions (PKG_GCC_LIBS / PKG_GLIBC in docker/steamos-baseline.env);
# the build FAILS on drift, since Valve can move them within a branch. This is not the release
# toolchain: the shipped launcher is built by the Flatpak SDK's GCC 14.
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

# Stage 2: the SteamOS rootfs alone.
FROM scratch
COPY --from=rootfs /rootfs/ /

# Recorded so the sim runner can assert what the container is rather than trust the image tag.
ARG DECKBACK_STEAMOS_BRANCH=3.8.1x
ARG DECKBACK_STEAMOS_GCC=15.1.1+r7+gf36ec88aa85a-1
ENV DECKBACK_SIM_BASE=steamos \
    DECKBACK_SIM_BRANCH=${DECKBACK_STEAMOS_BRANCH} \
    DECKBACK_SIM_GCC=${DECKBACK_STEAMOS_GCC}

# Explicit, so CMake cannot pick up a different compiler.
ENV CC=gcc \
    CXX=g++

# bwrap needs unprivileged user namespaces; under AppArmor's userns restriction (Ubuntu >= 23.10)
# seccomp=unconfined alone is not enough, so scripts/sim/run.sh passes the full option set.
# Source is bind-mounted at /src, never baked in.
WORKDIR /src
