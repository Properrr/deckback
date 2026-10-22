# Containerized test sim (SteamOS userspace in Docker)

`just sim [all|launcher|shortcut|portal|reconnect]` → `scripts/sim/run.sh` → `incontainer.sh`, on
`docker/steamos.Dockerfile`. It runs the GPU-independent layers of the on-Deck tests in a container
whose userspace matches the Deck exactly. It is a slot between L1 `smoke` and L2 on-Deck, not a
replacement for L2.

## The one rule: the sim cannot PASS a hardware gate

A container faithfully runs some layers and CANNOT run the ones that gate a release:

- **VA-API hardware decode** (`VaapiVideoDecoder`, the green-band pixel verdict) needs the Van
  Gogh/Sephiroth APU + radeonsi + ANGLE's DMA-buf import. lavapipe is software and has no VA-API —
  and this is the exact path that false-passed every automated metric on m114 while a human saw the
  corruption.
- **Power** (no battery → the `0.00 W … PASS` trap), **suspend/resume** (no ACPI/`rtcwake`), EPP, the
  real Deck APU (LCD vs OLED), true Game-Mode/Steam-session semantics, and touchscreen behaviour
  under gamescope are equally out of reach.

So `run.sh` refuses `gpu|vaapi|decode|power|soak|resume|suspend|pixel` by name with exit **6
(UNSUPPORTED-IN-SIM)**, never a 0, and every run prints the coverage boundary.
`tests/harness/test_sim_guardrail.sh` pins the refusals, that `run.sh` builds the SteamOS image, that
`incontainer.sh` refuses (exit 3) unless `DECKBACK_SIM_BASE=steamos`, that all three Docker flags
below are granted, and that the toolchain pin fails the build on drift.

## The image is real SteamOS, pinned to the Deck

`docker/steamos.Dockerfile` bootstraps a pure SteamOS rootfs from Valve's mirror
(`https://steamdeck-packages.steamos.cloud/archlinux-mirror/$repo/os/$arch`, branch **`3.8.1x`**:
`jupiter-3.8.1x`, `holo-3.8.1x`, `core-3.8.1x`, …); archlinux is used only to run `pacman`, and
nothing from it survives into the image. `archlinux:latest` is not a substitute: it is a rolling
release that runs ahead of SteamOS's frozen snapshot (measured: gcc-libs 16.1.1 vs the Deck's
15.1.1, systemd-libs 261.1 vs 257.7, flatpak 1.18.0 vs 1.16.6), so a green there answers a different
question.

Every installed package resolves to the exact version string the Deck reports (SteamOS **3.8.15**,
build 20260715.1, Galileo/OLED): glibc `2.41+r65+ge7c419a29575-1`, gcc `15.1.1+r7+gf36ec88aa85a-1`,
systemd-libs `257.7-2.5`, curl `8.15.0-1`, libpulse `17.0+r43+g3e2bb8a1e-1`, libxcb `1.17.0-1`,
libevdev `1.13.4-1`, flatpak `1:1.16.6-1.1`, bubblewrap `0.11.0-1`.

**The repos and versions are read off the device, never guessed.**
`scripts/sim/capture-deck-baseline.sh` pulls the unit's `/etc/os-release`, package versions,
`/etc/pacman.conf` and mirrorlist into `docker/steamos-baseline.env`.

**What the pin is for:** reproducibility, not the release toolchain. The Deck never compiles
anything; the shipped launcher is built by the Flatpak SDK's GCC (25.08 SDK: GCC 15.2.0) against that
runtime. The Dockerfile **fails the build on drift** (`PIN DRIFT: gcc is X, pinned to Y`) — verified
with a bogus pin.

## Suites

- **`launcher`** — builds the out-of-tree launcher against the Deck's own gcc/glibc/libsystemd and
  runs its L0 tests. `systemd-libs` ships `sd-bus.h`, so this is the real `PortalUpdater`, not the stub.
- **`shortcut`** — the installer's Steam-tile writing (`steam_shortcuts.py add` + `art`) produces a
  correct `shortcuts.vdf` entry and the five `grid/<crc-appid>*.png` files against a synthetic Steam
  userdata layout.
- **`portal`** — `flatpak-portal` + the PermissionStore activate in-container and enforce their
  caller contract: a non-flatpak caller gets `NotSupported: "Updates only supported by flatpak apps"`
  from `CreateUpdateMonitor`, so the test caller must present `/.flatpak-info`.
- **`reconnect`** — drives the launcher's real D-Bus reconnect logic (`durable/dbus-reconnect.md`).
  `deckback-launcher --selftest-watch <secs>` (`launcher/src/simwatch.{cpp,hpp}`, using only the public
  `Updater` start()/stop() interface) runs the real `PortalUpdater` loop inside a bwrap
  flatpak-instance (synthetic `/.flatpak-info`; a self-managed `dbus-daemon` at a fixed socket path so
  the bus can be dropped and restored at the same address). Four asserts: positive control (monitor
  comes up), case B (`pkill flatpak-portal` + reactivate → "flatpak-portal restarted — re-creating the
  update monitor"), case A drop (kill the daemon → "session bus error — attempting to reconnect"), case
  A recover (restart it → "reconnected to the Flatpak portal"). It refuses to pass a stub build.

## Required Docker flags (measured minimum; `--privileged` adds nothing)

| flag | without it |
|---|---|
| `--security-opt seccomp=unconfined` | Docker's default seccomp blocks `unshare` |
| `--security-opt apparmor=unconfined` | `bwrap: Failed to make / slave: Permission denied` (hosts with `kernel.apparmor_restrict_unprivileged_userns = 1`, Ubuntu ≥ 23.10; WSL2 does not set it) |
| `--cap-add SYS_ADMIN` | `bwrap: setting up uid map: Permission denied` |

With all three, all four suites are green on a native Linux host.

## Open

- `incontainer.sh` maps every `bad()` to exit 2; a bwrap privilege denial is an environment failure
  and should exit 3 (HARNESS §1).
- The full self-update **deploy** round-trip (staging-repo commit → `Update()` → Done) needs an ostree
  repo fixture.
- **`sim-gamescope`** (L1.7, not built): `gamescope --backend headless` on lavapipe to test
  compositor/DOM plumbing — overlay injection under a real Wayland compositor, capture⇔paint,
  Leanback body-swap survival, evdev input via uinput — with every render result stamped "software
  render — NOT the Deck GPU path". Container stability of gamescope-headless on lavapipe is unproven.
- The baseline records **one unit** (Galileo/OLED); an LCD (Jupiter) baseline would differ and has
  never been captured.

## After every SteamOS update

Re-run `scripts/sim/capture-deck-baseline.sh` against a Deck. **The git diff of
`docker/steamos-baseline.env` is the upgrade note.** Bump `DECKBACK_STEAMOS_BRANCH` /
`DECKBACK_STEAMOS_GCC` / `DECKBACK_STEAMOS_GLIBC` in `docker/steamos.Dockerfile` to match, then re-run
`just sim all` before trusting any prior green. The build refuses an unbumped pin.
