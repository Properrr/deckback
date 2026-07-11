---
scope: durable
topic: flatpak-packaging
verified: 2026-07-08
---

# Flatpak / Game Mode packaging (Phase 8)

> **SUPERSEDED 2026-07-10 for status; KEEP for architecture.** The current, authoritative packaging
> facts are in [`packaging-toolchain.md`](packaging-toolchain.md): the runtime is now
> **`25.08`** (not 24.08), `--device=input` and the metainfo file are **shipped** (not deferred —
> the `pack` image moved to `debian:13-slim` so its flatpak-builder is new enough), and the sandbox
> was read back as `devices=dri;input;` from a real install (2026-07-10). The *architecture* notes
> below (zypak wraps `content_shell`, Chromium staged as a prebuilt, flatpak-builder-in-Docker
> requirements) are still accurate. Trust `packaging-toolchain.md` on any conflict.

The app ships as a Flatpak (`io.github.properrr.deckback`) on `org.freedesktop.Platform//25.08`,
built **host-side in the `pack` container** (Deck stays deploy-only), installed `--user` on the Deck,
and surfaced in Steam via `steamos-add-to-steam <exported .desktop>`. `just flatpak` → `just install`.

## Architecture
- **Chromium isn't built in flatpak-builder.** `content_shell` + its runtime resources (paks, ICU, V8
  snapshots, locales, ANGLE/SwiftShader libs) are staged as a prebuilt from `out/deck` into
  `flatpak/cobalt-prebuilt/` and copied wholesale into `/app/cobalt` (Chromium resolves resources
  relative to the binary). zypak and the small launcher DO compile against the SDK.
- **zypak wraps content_shell, not the launcher.** The launcher (watchdog / CDP UA-injection / power)
  runs OUTSIDE the sandbox and forks `/app/bin/cobalt-zypak`, a shim that does
  `exec zypak-wrapper.sh /app/cobalt/content_shell "$@"`. zypak must wrap the Chromium binary directly
  for its zygote/sandbox broker to intercept. Confirmed working: `flatpak run` starts a real
  gpu-process with no SUID-sandbox error, and the launcher drives it to Leanback under the TV UA.
- `DECKBACK_COBALT_BIN=/app/bin/cobalt-zypak` is set via manifest `--env`.

## flatpak-builder in Docker (the `pack` service) — non-obvious requirements
- **`systempaths=unconfined`** (plus `cap_add: SYS_ADMIN`, unconfined seccomp/apparmor). Docker masks
  `/proc` subpaths with locked read-only mounts; that makes flatpak-builder's nested bwrap fail with
  `Can't mount proc ... Operation not permitted`. Unmasking system paths fixes it. `--disable-rofiles-fuse`
  avoids needing `/dev/fuse`.
- **elfutils** (`eu-strip`) — flatpak-builder splits debuginfo with it; absent → build dies.
- **librsvg2-common** — registers the gdk-pixbuf SVG loader so `flatpak build-export` can validate the
  scalable icon; absent → `not a valid icon: Format not recognized`.
- Runtime/SDK download is persisted to a **host bind-mount** via `XDG_DATA_HOME=/cache/flatpak/share`
  (pre-create it owned by uid 1000, like sccache — Docker auto-creates bind sources as root).

## zypak build (v2025.09)
Plain `Makefile` (its `./configure` is a no-op), NOT CMake/meson. Module = `buildsystem: simple` with
`make` + `make install` (installs into `$FLATPAK_DEST`). Ships `zypak-wrapper.sh` **and** a
`zypak-wrapper` symlink. Submodules (nickle, doctest) are fetched by flatpak-builder automatically.

## Deferred (Debian-12 pack image has an old flatpak/flatpak-builder)
- **`--device=input`** — flatpak 1.14 rejects that device type at `build-finish` (valid: dri/all/kvm/shm).
  Not baked into finish-args; `just install` grants it on the Deck (flatpak 1.16) via
  `flatpak override --user --device=input`. Never `--device=all`. Not needed until Phase 3 input.
- **appstream metainfo** — flatpak-builder 1.2.3 invokes the retired `appstream-compose` (24.08 SDK ships
  `appstreamcli`). Metainfo file exists (`flatpak/assets/…metainfo.xml`) but is NOT installed yet; wire
  back for Flathub once the pack image ships a newer builder.

## finish-args (v1)
`--share=network`, `--socket=x11` (gamescope Xwayland; content_shell `--ozone-platform=x11`),
`--socket=pulseaudio`, `--device=dri`, `--talk-name=org.freedesktop.ScreenSaver`,
`--system-talk-name=org.freedesktop.login1`, `--filesystem=xdg-run/pipewire-0:ro`. No `--no-sandbox`
(zypak owns the sandbox); the dev-only `--no-sandbox` path is `run.sh` + `DECKBACK_EXTRA_ARGS` only.
