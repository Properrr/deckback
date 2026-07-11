# P8 packaging toolchain — design (2026-07-10)

*Task #19. Every claim below was measured on this machine on 2026-07-10, with a negative control
where one was possible. Nothing here is quoted from documentation.*

## The problem, stated precisely

Two shipped decisions were written as "we can't", and both blamed the wrong thing:

  * `finish-args` cannot say `--device=input`, so the gamepad reader and the touchscreen
    `EVIOCGRAB` depend on `just install` running `flatpak override --device=input` afterwards.
  * `io.github.properrr.deckback.metainfo.xml` is built, shipped in `flatpak/assets/`, and
    deliberately **not installed**, because installing it kills the build.

Neither is a limitation of Flatpak, the runtime, or the Deck. Both are limitations of **one Debian 12
package set** in the `pack` image, which is the only image in this repo that has no reason to be
Debian 12 at all.

| | `deckback-pack` (Debian 12) | `debian:13-slim` (trixie) |
|---|---|---|
| flatpak | 1.14.10 | **1.16.6** (= the Deck's) |
| flatpak-builder | 1.2.3 | **1.4.4** |
| appstreamcli | absent | 1.0.5 |

Measured, both directions:

```
$ flatpak build-finish --device=input $d      # 1.14.10
error: Unknown device type input, valid types are: dri, all, kvm, shm
$ flatpak build-finish --device=input $d      # 1.16.6
  -> metadata: [Context] devices=dri;input;
```

```
$ flatpak-builder ... test.yml                # 1.2.3, metainfo installed
Renaming io.github.properrr.deckback.metainfo.xml to share/appdata/...appdata.xml
Running appstream-compose
bwrap: execvp appstream-compose: No such file or directory
Error: ERROR: appstream-compose failed
$ flatpak-builder ... test.yml                # 1.4.4, same manifest
Exporting share/metainfo/io.github.properrr.deckback.metainfo.xml
RC=0     # and bd/files/share/app-info/xmls/io.github.properrr.deckback.xml.gz exists
```

flatpak-builder 1.2.3 renames the file to the retired `appdata` location and shells out to
`appstream-compose`, a binary the 24.08 SDK no longer carries. 1.4.4 keeps `metainfo/` and uses
`appstreamcli compose`. The runtime was never the problem.

`--device=input` was added in flatpak **1.15.6** (upstream `NEWS`), so **1.16.0** is the first stable
release that understands it — that is a fact, and it is what `--require-version` will say. It also
explains 1.14.10 exactly, rather than by coincidence.

## D1 — `pack` leaves Debian 12; `build`/`test`/`dev` stay

`CLAUDE.md` requires the *engine* image to match Cobalt trunk's `debian12`. `pack` never compiles
Chromium and never compiles the launcher on the host: flatpak-builder builds every module **inside
the `org.freedesktop.Sdk` sandbox**, with the SDK's own GCC. The host distro contributes
`flatpak-builder` and nothing else to the artifact. So `pack` becomes its own `FROM`, on
`debian:13-slim`, via a `PACK_BASE` build arg.

This is the whole unlock. D3 and D4 follow from it for free.

## D2 — runtime and SDK 24.08 → 25.08

Not for features. For support life, and the evidence is dated:

```
org.freedesktop.Platform//22.08  last update 2025-09-08   End-of-life: ... no longer receiving fixes
org.freedesktop.Platform//23.08  last update 2025-09-29   End-of-life: ... no longer receiving fixes
org.freedesktop.Platform//24.08  last update 2026-07-02   <no EOL marker>
org.freedesktop.Platform//25.08  last update 2026-07-08   <no EOL marker>
```

Two branches are supported at a time, and 23.08 was retired when 25.08 shipped. 26.08 is due in
August 2026 — **next month** — and on that pattern 24.08 goes EOL with it. Moving now, on a quiet
week, is cheaper than moving in a hurry after the EOL banner appears in `flatpak update`.

What the upgrade does *not* buy, so nobody claims it later:

  * **Not newer Mesa.** The VA-API driver comes from `org.freedesktop.Platform.GL.default`, not from
    `Platform`, and `GL.default//24.08` already carries Mesa 26.0.6 (25.08: 26.1.4). The GL extension
    tracks Mesa independently of the Platform branch.
  * **Not codecs.** Our `content_shell` has no `libavcodec` in `NEEDED` and there is no
    `libffmpeg.so` in `out/deck`: ffmpeg is statically linked. `Platform.ffmpeg-full` (24.08 only)
    and its 25.08 successor `Platform.codecs-extra` are both irrelevant to us. This also means the
    `ffmpeg-full` extension's absence from 25.08 is *not* a blocker, which is the one thing that
    would have blocked the move.
  * **Not glibc.** 2.40 → 2.42, and Chromium links against its own hermetic sysroot.

Risks, both real: the GL extension branch is spelled `24.08extra` but `25.08-extra` (hyphen), and the
25.08 SDK's GCC is newer than 14.3.0, which may surface new `-Wall -Wextra -Werror` diagnostics in
`launcher/`. Both are found by building, which is the next step, not by reading.

All 30 of `content_shell`'s `NEEDED` libraries are present in `Platform//25.08` + `GL.default//25.08`
(and in 24.08). Re-verified today, not taken from a note.

## D3 — bake `--device=input`, and make its absence loud

`finish-args` gains `--device=input` (never `--device=all`). `scripts/install.sh` drops the
`flatpak override` — and, more importantly, drops this:

```sh
flatpak override --user --device=input ... 2>/dev/null \
  && info "Granted --device=input" \
  || info "note: --device=input override not applied (older flatpak?)"
```

That is F1 again. The failure branch prints a note and returns 0, and the user gets an app whose
gamepad does not work and whose touchscreen lock is a silent no-op. Replaced by a check that reads
the *installed* metadata and fails.

## D4 — install the metainfo, and validate it

`appstreamcli validate` on our existing file today:

```
W: url-homepage-missing
I: developer-info-missing
Validation failed: warnings: 1, infos: 1      (rc=3)
```

So the file goes in, the two gaps get filled, and `appstreamcli validate` + `desktop-file-validate`
become gates in `just flatpak` — before the 1.2 GB build, not after.

## D5 — `--share=ipc`

Chromium on X11 uses MIT-SHM. Without `--share=ipc` the shared-memory path fails and it falls back to
socket transport for every frame. Every Chromium-based Flathub app pairs `--socket=x11` with
`--share=ipc`. We render 1280x720 at 60 fps on a handheld; this is not a rounding error.

## D6 — a bundle a fresh Deck can actually install

`flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo` embeds the
runtime's origin, so `flatpak install --user deckback.flatpak` on a Deck that has never seen
`org.freedesktop.Platform//25.08` offers to fetch it instead of failing. Without it the bundle only
installs on a machine that already happens to have the runtime.

Also `flatpak build-update-repo --generate-static-deltas --prune` on the local repo.

## Deck-specific `finish-args`, reviewed one by one

Kept, each for a reason we can name:

| arg | why |
|---|---|
| `--socket=x11` + `--share=ipc` | Game Mode is gamescope's Xwayland; MIT-SHM (D5) |
| `--device=dri` | VA-API decode |
| `--device=input` | evdev gamepads, FF_RUMBLE haptics, touchscreen `EVIOCGRAB` |
| `--socket=pulseaudio` | PipeWire's pulse compat — playback *and* mic capture (P5 voice) |
| `--filesystem=xdg-run/pipewire-0:ro` | direct PipeWire for the audio module |
| `--system-talk-name=org.freedesktop.login1` | logind sleep/resume watcher (P6) |
| `--talk-name=org.freedesktop.ScreenSaver` | idle inhibit during playback |
| `--share=network` | it is a video client |
| `--require-version=1.16.0` | the release that learned `--device=input`; a 1.14 host gets a clear error instead of a broken app |

Rejected, so nobody adds them later thinking they were forgotten:

  * `--device=all` — grants every device to get one. `--device=input` is the whole need.
  * `--no-sandbox` — Flathub rejects it and zypak exists precisely so we never need it.
  * `--filesystem=home` — the CDM and the profile live in `~/.var/app/<id>`, which we already own.
  * `--socket=wayland` — `content_shell` runs `--ozone-platform=x11`. Adding a socket we do not use
    widens the sandbox for nothing.
  * `Platform.ffmpeg-full` / `codecs-extra` — static ffmpeg (D2).
  * `LIBVA_DRIVERS_PATH` — measured unset inside the sandbox and *not needed*: libva's compiled-in
    default already includes `/usr/lib/x86_64-linux-gnu/GL/lib/dri`, where `GL.default` puts
    `radeonsi_drv_video.so`. Setting it would be cargo cult.

## What "the builder has all necessary features" means concretely

`pack` must carry: `flatpak` ≥1.16, `flatpak-builder` ≥1.4, `elfutils` (eu-strip splits debuginfo),
`librsvg2-common` (gdk-pixbuf SVG loader, or `build-export` rejects the scalable icon), `appstream`
(`appstreamcli` — validate + compose), `desktop-file-utils` (`desktop-file-validate`), `ostree`
(inspect the repo), `git` + `ca-certificates` (the zypak git source).

A version floor is asserted at image build time, not discovered at 1.2 GB into a package run.

---

# What executing the design taught us (2026-07-10)

Everything above was written before `just flatpak` was run once against the new toolchain. Three
things only the run could say.

## The 25.08 SDK is GCC 15.2.0, and the launcher is clean under it

`-Wall -Wextra -Werror`, C++23, no new diagnostics. This was the upgrade's real risk (24.08 = GCC
14.3.0) and it cost nothing. `libsystemd 257`, `libcurl 8.15.0`, `libpulse 17.0` all resolve inside
the SDK, so every optional dependency of `launcher/` is compiled in rather than silently skipped.

## A regression I had just written, found only by running

`launcher/CMakeLists.txt` reads `../config/av1_steering.js` at configure time and embeds it. That
path exists in the repo. It does not exist inside flatpak-builder, which copies each module's sources
**flat** into `/run/build/<module>/`. The build died with CMake's own message:

```
file failed to open for reading (No such file or directory):
  /run/build/deckback-launcher/../config/av1_steering.js
```

which names neither the module nor the reason. The generator was added earlier the same day; every
unit test passed, `just launcher build` passed, and `just flatpak` had not been run since. The fix is
a cache variable (`-DDECKBACK_AV1_JS`) plus a second source with `dest-filename`, and a
`FATAL_ERROR` that says what the file is for:

> Without it the launcher would ship with no AV1 steering, which **fails open**: YouTube serves AV1
> and the Deck decodes it in software.

That last clause is the point. A missing steering script does not crash — it produces a Deck that
plays video at twice the power draw. The check exists because the failure is quiet.

## The post-condition earned its place immediately

`scripts/flatpak.sh` now reads the sandbox back out of the committed ostree object and refuses to
bundle an app that cannot see `/dev/input`. On the first successful run it printed:

```
packaged context: shared=network;ipc; sockets=x11;pulseaudio; devices=dri;input;
```

and the bundle, installed into a **fresh** flatpak user dir inside the pack container, reported:

```
$ flatpak info --show-permissions io.github.properrr.deckback
[Context]
shared=network;ipc;
sockets=x11;pulseaudio;
devices=dri;input;
filesystems=xdg-run/pipewire-0:ro;
```

That install also validated D6 without being asked to: the dir had never seen the runtime, and
`--runtime-repo` pulled `org.freedesktop.Platform//25.08`, `GL.default//25.08`, `GL.default//25.08-extra`
and `codecs-extra//25.08-extra` from Flathub on its own.

Getting there required adding `dbus` to the pack image. Without a session bus `flatpak install` fails
with `Could not connect: No such file or directory`, so the bundle could be *built* but never
*installed* — and therefore never inspected. An image that can produce an artifact it cannot check is
half a toolchain.

## `--device=all` must be *recognised*, though never *requested*

`scripts/lib.sh:flatpak_grants_input` accepts `devices=all;`. Our manifest may never ask for it
(`flatpak-lint.sh` fails the build if it does), but a user who granted it by hand has a working
gamepad, and `just install` must not tell them their install is broken. The same function ignores
every line but `devices=`, so `filesystems=/mnt/input` cannot be mistaken for a device. Both cases
have a test.

## The gates are mutation-tested, because a gate nobody can fail is decoration

Each `flatpak-lint.sh` check was run against a deliberately broken manifest and observed to fail with
the right message: `--device=all` substituted for `--device=input`; `--share=ipc` deleted;
`--require-version` deleted; the metainfo install line deleted; a real `--no-sandbox` added to
`finish-args`. The first version of the `--no-sandbox` gate fired on the *comments explaining why
`--no-sandbox` is absent* — a gate that cries wolf on its own documentation is deleted within the
week, so it now matches arguments rather than prose.

The `flatpak_grants_input` tests were mutation-tested too: dropping the `all` case, parsing the whole
blob instead of the `devices=` line, and inverting the verdict are each caught.

## Corrections to the design above

  * The `pack` stage needs `dbus`, which D-nothing predicted.
  * `appstreamcli validate` must run with **`--no-net`**. Left to itself it resolves every `<url>`
    over the network and reports `url-not-reachable` — so the gate would fail because GitHub is slow,
    or because this project is not published yet. It failed for exactly that reason on first run,
    after the `homepage`/`developer` gaps were fixed. A check that fails for reasons unrelated to
    what it checks gets skipped, and a skipped check checks nothing.
  * `ARG PACK_BASE` has to be declared **before the first `FROM`**. Inside the stage it is too late,
    and the error — `base name (${PACK_BASE}) should not be blank` — names neither the arg nor the
    rule.
