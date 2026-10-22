# Packaging toolchain — current facts

*Measured on the workstation and on a real install, 2026-07-10, with a negative control where one was
possible. Architecture (zypak, prebuilt staging, pack-container requirements) is in
[`packaging.md`](packaging.md).*

## The `pack` image is `debian:13-slim`, not Debian 12

`CLAUDE.md` requires the *engine* images to match Cobalt trunk's `debian12`. `pack` is the exception
because it never compiles Chromium or the launcher on the host: flatpak-builder builds every module
**inside the `org.freedesktop.Sdk` sandbox** with the SDK's own GCC. The host distro contributes
`flatpak-builder` and nothing else to the artifact. `pack` is its own `FROM ${PACK_BASE}` stage
(`PACK_BASE=debian:13-slim`).

Debian 12's package set cannot build what we ship:

| | Debian 12 | `debian:13-slim` (trixie) |
|---|---|---|
| flatpak | 1.14.10 | **1.16.6** (= the Deck's) |
| flatpak-builder | 1.2.3 | **1.4.4** |
| appstreamcli | absent | 1.0.5 |

- **`--device=input`** was added in flatpak **1.15.6** (upstream `NEWS`), so **1.16.0** is the first
  stable release that understands it. 1.14.10 rejects it at `build-finish`
  (`Unknown device type input, valid types are: dri, all, kvm, shm`); 1.16.6 writes
  `devices=dri;input;`. The manifest therefore carries `--require-version=1.16.0`.
- **Metainfo.** flatpak-builder 1.2.3 renames the metainfo to the retired `appdata` location and runs
  `appstream-compose`, which the SDK no longer carries (`bwrap: execvp appstream-compose: No such file
  or directory`). 1.4.4 keeps `share/metainfo/` and uses `appstreamcli compose`.
- **`ARG PACK_BASE` must be declared before the first `FROM`.** Inside a stage it is too late, and
  the error (`base name (${PACK_BASE}) should not be blank`) names neither the arg nor the rule.

What `pack` must carry, with the floor asserted at image build time: `flatpak` ≥1.16,
`flatpak-builder` ≥1.4, `elfutils` (eu-strip), `librsvg2-common` (SVG icon loader), `appstream`
(`appstreamcli` validate + compose), `desktop-file-utils`, `ostree`, `git` + `ca-certificates` (the
zypak git source), and **`dbus`** — without a session bus `flatpak install` fails with
`Could not connect: No such file or directory`, so a bundle could be built but never installed and
inspected.

## Rootless Docker: the icon validator runs through a bwrap wrapper

Under rootless Docker, bwrap cannot bring up loopback in a network namespace nested under
rootlesskit's, so `flatpak-validate-icon` (which flatpak runs with `--unshare-net`) fails.
`scripts/lib.sh:compose_rootless_args` detects rootless Docker and sets
`FLATPAK_BWRAP=/work/docker/rootless/bwrap`; that wrapper drops `--unshare-net` for
`flatpak-validate-icon` only and passes every other bwrap call through untouched. The same function
runs the container as `--user 0:0` and loads `docker/rootless/sitecustomize.py` (via `PYTHONPATH`) plus
`TAR_OPTIONS=--no-same-owner`, because container root cannot chown to unmapped uids and Python's
`tarfile` then silently skips the chmod (executables lose their exec bit).

## Runtime and SDK: 25.08

Chosen for support life: two Platform branches are supported at a time, and 24.08 goes EOL when
26.08 ships. The 25.08 SDK is **GCC 15.2.0**; `launcher/` builds clean under it (`-Wall -Wextra
-Werror`, C++23), and `libsystemd 257`, `libcurl 8.15.0`, `libpulse 17.0` all resolve in the SDK, so
every optional launcher dependency is compiled in.

What the runtime does *not* determine, so nobody claims it later:

- **Not Mesa.** The VA-API driver comes from `org.freedesktop.Platform.GL.default`, which tracks Mesa
  independently of the Platform branch. Its extra branch is spelled `25.08-extra` (hyphen; 24.08 was
  `24.08extra`).
- **Not codecs.** ffmpeg is statically linked into `content_shell` (no `libavcodec` in `NEEDED`, no
  `libffmpeg.so`), so `Platform.ffmpeg-full` / `codecs-extra` are irrelevant.
- **Not glibc.** Chromium links against its own hermetic sysroot.

All 30 of `content_shell`'s `NEEDED` libraries are present in `Platform//25.08` + `GL.default//25.08`.

## `finish-args`

| arg | why |
|---|---|
| `--require-version=1.16.0` | first stable flatpak that knows `--device=input`; a 1.14 host gets a clear error instead of a broken app |
| `--socket=x11` + `--share=ipc` | Game Mode is gamescope's Xwayland (`--ozone-platform=x11`); Chromium's X11 backend uses MIT-SHM, which falls back to socket transport per frame without `--share=ipc` |
| `--device=dri` | VA-API decode |
| `--device=input` | evdev gamepads, FF_RUMBLE haptics (it does not make the touchscreen openable — `touch-lock.md`) |
| `--socket=pulseaudio` | PipeWire's pulse compat — playback and mic capture |
| `--filesystem=xdg-run/pipewire-0:ro` | direct PipeWire for the audio module |
| `--system-talk-name=org.freedesktop.login1` | logind sleep/resume watcher |
| `--talk-name=org.freedesktop.ScreenSaver` | idle inhibit during playback |
| `--talk-name=org.freedesktop.portal.Flatpak` | self-update through the portal (`self-update.md`) |
| `--talk-name=org.freedesktop.impl.portal.PermissionStore` | the self-update consent seed (`self-update.md`) |
| `--share=network` | it is a video client |

**No permission may be added once users are installed**: flatpak-portal refuses a self-update that
widens the sandbox (`self-update.md` §"Permission adds break self-update"); `check-permissions.sh`
in `just preflight` gates it.

Rejected, so nobody adds them later thinking they were forgotten:

- `--device=all` — grants every device to get one.
- `--no-sandbox` — Flathub rejects it and zypak exists so we never need it.
- `--filesystem=home` — the CDM and the profile live in `~/.var/app/<id>`, which we already own.
- `--socket=wayland` — `content_shell` runs `--ozone-platform=x11`; an unused socket only widens the
  sandbox.
- `Platform.ffmpeg-full` / `codecs-extra` — static ffmpeg.
- `LIBVA_DRIVERS_PATH` — measured unset inside the sandbox and not needed: libva's compiled-in
  default includes `/usr/lib/x86_64-linux-gnu/GL/lib/dri`, where `GL.default` puts
  `radeonsi_drv_video.so`.

## Gates in `just flatpak`

- **`flatpak-lint.sh` runs first**, before the build (`appstreamcli compose` only runs at the very
  end of flatpak-builder). It fails on: a YouTube/YT app id, a real `--no-sandbox` argument (it
  matches arguments, not comments), `--device=all`, a missing `--device=input`, a missing
  `--require-version=1.16.x`, a missing `--share=ipc`, an uninstalled metainfo, and missing metainfo
  screenshots. The `--device`, `--share=ipc`, `--require-version`, metainfo and `--no-sandbox`
  checks were each mutation-tested against a deliberately broken manifest.
- **`appstreamcli validate --no-net`** + `desktop-file-validate`. Without `--no-net` appstreamcli
  resolves every `<url>` and fails with `url-not-reachable` for reasons unrelated to the metadata.
- **Post-condition:** `scripts/flatpak.sh` reads the sandbox back out of the committed ostree object
  and refuses to bundle an app that cannot see `/dev/input`. A good run prints
  `packaged context: shared=network;ipc; sockets=x11;pulseaudio; devices=dri;input;`, and a fresh
  install reports the same via `flatpak info --show-permissions` (verified 2026-07-10).
- **`scripts/install.sh`** reads the *installed* metadata and fails if input is not granted, rather
  than printing a note and returning 0. `lib.sh:flatpak_grants_input` accepts `devices=all;` (a user
  who granted it by hand has a working gamepad) and reads only the `devices=` line, so
  `filesystems=/mnt/input` is not mistaken for a device; both cases are tested and mutation-tested.

## The bundle and repo

- `flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo` embeds the
  runtime's origin, so `flatpak install --user deckback.flatpak` on a Deck that has never seen the
  runtime fetches it instead of failing. Verified: a fresh user dir pulled `Platform//25.08`,
  `GL.default//25.08`, `GL.default//25.08-extra` and `codecs-extra//25.08-extra` from Flathub.
- The local repo is updated with `flatpak build-update-repo --generate-static-deltas --prune`. The
  published repo's history and deltas are `scripts/publish-repo.sh` (`self-update.md`).

## flatpak-builder copies each module's sources flat

Sources land in `/run/build/<module>/`, so a repo-relative path such as `../config/...` does not
exist inside the build. The launcher module therefore receives `config/scripts/` as a `type: dir`
source and is configured with `-DDECKBACK_SCRIPTS_DIR=scripts`; CMake fails the build if the
directory is missing, because a launcher without its page scripts fails open (no AV1 steering means
YouTube serves AV1 and the Deck decodes it in software) rather than crashing.
