# syntax=docker/dockerfile:1.7
#
# Debian 12 (bookworm) to match Cobalt trunk, which builds every image on
# marketplace.gcr.io/google/debian12. The hermetic Chromium toolchain needs glibc (no Alpine/musl).
# The package lists are MIRRORED from cobalt/docker/{linux,unittest}/Dockerfile at the pinned
# commit: re-sync them on every Cobalt bump (.internal/MIGRATION.md). Byte-identical upstream base:
#   BASE=marketplace.gcr.io/google/debian12@sha256:be15f84c44c4ee68b4f187128f0278df1b0f424c04fb5f08b098967d5b896388

ARG BASE=debian:12-slim
# Global scope (before the first FROM) so the `pack` stage's FROM can use it.
ARG PACK_BASE=debian:13-slim

# ---- build: engine build toolchain (smallest — this is what CI uses) ----
FROM ${BASE} AS build
ENV DEBIAN_FRONTEND=noninteractive LANG=en_US.UTF-8
# Mirrored from cobalt/docker/linux/Dockerfile (minus the gcloud CLI) + host tooling for
# depot_tools / deploy (rsync, ssh, sudo, locales).
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl git python3 python3-dev xz-utils zstd bzip2 pkgconf jq \
      gperf gcc-multilib fuse-overlayfs \
      libglib2.0-0 libnss3 libdbus-1-3 libxcomposite1 libxdamage1 libxext6 libxfixes3 \
      libxrandr2 libxtst6 libgbm1 libasound2 libxkbcommon0 libpango-1.0-0 \
      binutils-aarch64-linux-gnu binutils-arm-linux-gnueabi \
      locales sudo rsync openssh-client \
    && sed -i 's/^# *en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen && locale-gen \
    && rm -rf /var/lib/apt/lists/*

# depot_tools (gclient, gn, autoninja); self-update off for hermeticity. gn/ninja/clang come from
# the checkout's hermetic toolchain via `gclient sync`, not apt.
RUN git clone --depth=1 https://chromium.googlesource.com/chromium/tools/depot_tools /opt/depot_tools
ENV PATH=/opt/depot_tools:$PATH DEPOT_TOOLS_UPDATE=0
# Login shells re-source /etc/profile, which resets PATH; profile.d runs after that reset.
RUN printf 'export PATH=/opt/depot_tools:$PATH\nexport DEPOT_TOOLS_UPDATE=0\n' \
      > /etc/profile.d/depot_tools.sh
# The `gn`/python3 wrappers need python3_bin_reldir.txt, which only self-update creates, so bootstrap
# depot_tools' python3 explicitly.
RUN cd /opt/depot_tools \
 && DEPOT_TOOLS_UPDATE=1 bash -c 'source ./bootstrap_python3 && bootstrap_python3' \
 && test -f /opt/depot_tools/python3_bin_reldir.txt

# sccache — persistent compile cache on a named volume.
ARG SCCACHE_VER=v0.10.0
RUN curl -fsSL "https://github.com/mozilla/sccache/releases/download/${SCCACHE_VER}/sccache-${SCCACHE_VER}-x86_64-unknown-linux-musl.tar.gz" \
    | tar xz --strip-components=1 -C /usr/local/bin --wildcards '*/sccache'
ENV SCCACHE_DIR=/cache/sccache SCCACHE_CACHE_SIZE=100G

# Non-root user mapped to host UID/GID for bind-mount permission sanity.
ARG UID=1000 GID=1000
RUN groupadd -g ${GID} dev && useradd -m -u ${UID} -g ${GID} -s /bin/bash dev \
 && echo 'dev ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/dev \
 && chown -R dev:dev /opt/depot_tools  # depot_tools self-bootstraps cipd/vpython into its own dir
USER dev
WORKDIR /src

# ---- test: headless X + media stack for `just smoke` and unit/web tests ----
# Mirrored from cobalt/docker/unittest/Dockerfile (only the packages not already in `build`).
FROM build AS test
USER root
RUN apt-get update && apt-get install -y --no-install-recommends \
      dbus-x11 ffmpeg libatomic1 xvfb xauth xcvt x11-xserver-utils \
      xserver-xorg-core xserver-xorg-video-dummy openbox xcompmgr \
      libegl1 libegl1-mesa libegl-mesa0 libgbm-dev libglapi-mesa libgles2-mesa libxcomposite-dev \
      libatk1.0-0 libatk-bridge2.0-0 libatspi2.0-0 \
    && rm -rf /var/lib/apt/lists/*
USER dev

# ---- dev: interactive debuggers/editors + our out-of-tree launcher toolchain ----
FROM test AS dev
USER root
RUN apt-get update && apt-get install -y --no-install-recommends \
      gdb lldb clangd quilt ripgrep fd-find shellcheck bash-completion \
      strace ltrace less neovim cmake ninja-build clang clang-format \
 && curl --proto '=https' --tlsv1.2 -fsSL https://just.systems/install.sh \
    | bash -s -- --to /usr/local/bin \
 && rm -rf /var/lib/apt/lists/*
USER dev

# ---- pack: flatpak-builder for Phase 8 ----
# Not Debian 12 and not `FROM dev`: flatpak-builder compiles every module inside the SDK sandbox, so
# the host distro never reaches the artifact. bookworm's flatpak (1.14) rejects --device=input and its
# flatpak-builder (1.2.3) cannot install a metainfo file (findings/durable/packaging-toolchain.md).
FROM ${PACK_BASE} AS pack
ENV DEBIAN_FRONTEND=noninteractive
ARG UID=1000 GID=1000
# elfutils:          eu-strip/eu-elfcompress — flatpak-builder splits debuginfo with these.
# librsvg2-common:   the gdk-pixbuf SVG loader, without which `build-export` rejects the scalable icon.
# appstream:         appstreamcli — `validate` (our gate) and `compose` (flatpak-builder's).
# desktop-file-utils: desktop-file-validate — the other half of that gate.
# ostree:            inspect the built repo without installing it.
# git, ca-certificates: the zypak module is a git source.
# dbus:              `flatpak install` needs a session bus (dbus-run-session) for the post-build check.
RUN apt-get update && apt-get install -y --no-install-recommends \
      flatpak flatpak-builder elfutils librsvg2-common \
      appstream desktop-file-utils ostree git ca-certificates xz-utils zstd python3 dbus \
 && rm -rf /var/lib/apt/lists/*

# Fail the image build below the floors: flatpak 1.16.0 (first stable with `--device=input`) and
# flatpak-builder 1.4.0 (first to use `appstreamcli compose` instead of `appstream-compose`).
RUN set -eu; \
    fp=$(flatpak --version | awk '{print $NF}'); \
    fb=$(flatpak-builder --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+'); \
    ver_ge() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]; }; \
    ver_ge "$fp" 1.16.0 || { echo "flatpak $fp < 1.16.0: no --device=input" >&2; exit 1; }; \
    ver_ge "$fb" 1.4.0  || { echo "flatpak-builder $fb < 1.4.0: appstream-compose is gone" >&2; exit 1; }; \
    command -v appstreamcli >/dev/null || { echo "appstreamcli missing" >&2; exit 1; }; \
    command -v desktop-file-validate >/dev/null || { echo "desktop-file-validate missing" >&2; exit 1; }; \
    command -v dbus-run-session >/dev/null || { echo "dbus-run-session missing" >&2; exit 1; }; \
    echo "pack: flatpak $fp, flatpak-builder $fb, $(appstreamcli --version)"

RUN groupadd -g ${GID} dev && useradd -m -u ${UID} -g ${GID} -s /bin/bash dev
USER dev
