# syntax=docker/dockerfile:1.7
#
# Base = Debian 12 (bookworm), to MATCH COBALT TRUNK. Upstream builds every image
# (cobalt/docker/{linux,unittest,webtest,pre-commit}) on `marketplace.gcr.io/google/debian12`
# (SHA-pinned). We default to `debian:12-slim` for Docker Hub portability; set BASE to the exact
# upstream digest below for byte-for-byte CI parity.
#
# Why Debian 12 and not Ubuntu / Alpine / distroless:
#   * Chromium's toolchain is HERMETIC — gclient hooks fetch a pinned clang + gn/ninja + a glibc
#     sysroot into the checkout. The base image is only a "tool caddy", so glibc is REQUIRED:
#     Alpine/musl breaks the downloaded sysroot; distroless has no package manager. Non-starters.
#   * Matching upstream's distro is the whole point of the plan ("the CI YAML is the docs") — it
#     makes our local builds byte-identical to Cobalt's CI and avoids fighting install-build-deps.
#   * We deliberately do NOT run build/install-build-deps.sh — upstream doesn't either; it installs
#     a small curated list. The lists below are MIRRORED from cobalt/docker/linux/Dockerfile and
#     cobalt/docker/unittest/Dockerfile at the pinned commit. **Re-sync on every Cobalt bump**
#     (.internal/MIGRATION.md, step "toolchain/image").
#
# Source parity (byte-identical to upstream CI):
#   BASE=marketplace.gcr.io/google/debian12@sha256:be15f84c44c4ee68b4f187128f0278df1b0f424c04fb5f08b098967d5b896388

ARG BASE=debian:12-slim
# Declared here, before the first FROM: an ARG only reaches a `FROM` line if it is in the global
# scope. `pack` is the only stage that uses it. See the `pack` stage for why it is not Debian 12.
ARG PACK_BASE=debian:13-slim

# ---- build: engine build toolchain (smallest — this is what CI uses) ----
FROM ${BASE} AS build
ENV DEBIAN_FRONTEND=noninteractive LANG=en_US.UTF-8
# Mirrored from cobalt/docker/linux/Dockerfile (minus gcloud CLI, which is upstream's GCS artifact
# uploader — we don't need it) + host tooling for depot_tools / deploy (rsync, ssh, sudo, locales).
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl git python3 python3-dev xz-utils zstd bzip2 pkgconf jq \
      gperf gcc-multilib fuse-overlayfs \
      libglib2.0-0 libnss3 libdbus-1-3 libxcomposite1 libxdamage1 libxext6 libxfixes3 \
      libxrandr2 libxtst6 libgbm1 libasound2 libxkbcommon0 libpango-1.0-0 \
      binutils-aarch64-linux-gnu binutils-arm-linux-gnueabi \
      locales sudo rsync openssh-client \
    && sed -i 's/^# *en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen && locale-gen \
    && rm -rf /var/lib/apt/lists/*

# depot_tools (gclient, gn, autoninja); self-update off for hermeticity. Note: gn/ninja/clang are
# NOT apt packages here — they come from the checkout's hermetic toolchain via `gclient sync`.
RUN git clone --depth=1 https://chromium.googlesource.com/chromium/tools/depot_tools /opt/depot_tools
ENV PATH=/opt/depot_tools:$PATH DEPOT_TOOLS_UPDATE=0
# Login shells (`bash -lc`, used by scripts/lib.sh) re-source /etc/profile, which resets PATH and
# would drop /opt/depot_tools. profile.d runs after that reset, so gclient stays on PATH there too.
RUN printf 'export PATH=/opt/depot_tools:$PATH\nexport DEPOT_TOOLS_UPDATE=0\n' \
      > /etc/profile.d/depot_tools.sh
# Bootstrap depot_tools' bundled python3 into the image. The `gn` and `python-bin/python3` wrappers
# read python3_bin_reldir.txt, which self-update creates on first use — but we keep self-update off
# (DEPOT_TOOLS_UPDATE=0) for hermeticity, so do it explicitly here (bootstrap_python3 ignores that
# flag). Without this, `gn gen` dies with "python3_bin_reldir.txt not found".
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
#
# NOT Debian 12, and NOT `FROM dev`. This is the one image in this repo with no reason to match
# Cobalt trunk: `pack` never compiles Chromium, and it never compiles the launcher on the host
# either — flatpak-builder builds every module INSIDE the org.freedesktop.Sdk sandbox with the SDK's
# own GCC. The host distro contributes flatpak-builder and nothing else to the artifact.
#
# Staying on bookworm cost us two shipped features, and both were misattributed in the manifest:
#
#   * bookworm's flatpak is 1.14.10, and `--device=input` arrived in flatpak 1.15.6 (upstream NEWS),
#     so build-finish rejected it: "Unknown device type input, valid types are: dri, all, kvm, shm".
#     The gamepad reader and the touchscreen EVIOCGRAB therefore depended on `just install` running
#     `flatpak override` afterwards.
#   * bookworm's flatpak-builder is 1.2.3, which renames metainfo to the retired `appdata` path and
#     shells out to `appstream-compose` — a binary the 24.08+ SDK no longer ships. Installing our
#     metainfo file killed the build, so we did not install it.
#
# trixie ships flatpak 1.16.6 (the same version the Deck runs) and flatpak-builder 1.4.4, which uses
# `appstreamcli compose`. Both problems are the package set, not Flatpak, the runtime, or the Deck.
# See .internal/findings/durable/packaging-toolchain.md.
FROM ${PACK_BASE} AS pack
ENV DEBIAN_FRONTEND=noninteractive
ARG UID=1000 GID=1000
# elfutils:          eu-strip/eu-elfcompress — flatpak-builder splits debuginfo with these.
# librsvg2-common:   registers the gdk-pixbuf SVG loader, or `build-export` rejects the scalable
#                    icon with "not a valid icon: Format not recognized".
# appstream:         appstreamcli — `validate` (our gate) and `compose` (flatpak-builder's).
# desktop-file-utils: desktop-file-validate — the other half of that gate.
# ostree:            inspect the built repo without installing it.
# git, ca-certificates: the zypak module is a git source.
# dbus:              `flatpak install` needs a session bus. Without it the bundle can be built but
#                    never installed here, so nothing could check that the artifact we ship actually
#                    grants the permissions the manifest asked for. dbus-run-session makes the
#                    post-build install check possible; that check is the point of the whole change.
RUN apt-get update && apt-get install -y --no-install-recommends \
      flatpak flatpak-builder elfutils librsvg2-common \
      appstream desktop-file-utils ostree git ca-certificates xz-utils zstd python3 dbus \
 && rm -rf /var/lib/apt/lists/*

# Assert the floors HERE, at image build time. Discovering that flatpak is too old to understand
# `--device=input` costs one line now; discovering it 1.2 GB into a package run costs an afternoon,
# and discovering it on a user's Deck costs a bug report about a dead gamepad.
#
# 1.16.0 is the first stable flatpak with `--device=input` (added in 1.15.6).
# 1.4.0 is the first flatpak-builder that uses `appstreamcli compose` instead of `appstream-compose`.
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
