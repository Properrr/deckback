# Containerized test sim (SteamOS/gamescope-in-Docker)

## ★ CORRECTION 2026-07-16 — the sim was not SteamOS, and `reconnect` had never run here. Both fixed.

Found while re-running the sim on a **native Linux host** (`7.0.0-27-generic`); the Phase 1/2 work
below was spiked and greened on **WSL2**, and that difference hid both problems.

### 1. The base was `archlinux:latest`, which is NOT SteamOS — so `launcher` proved less than it said

`docker/sim.Dockerfile` read `FROM archlinux:latest` under "Arch base — honouring 'SteamOS is
Arch-based'". True, and not enough: archlinux:latest is a ROLLING release drifting continuously ahead
of SteamOS 3.x, which is a FROZEN Arch snapshot plus Valve's holo/jupiter packages. Measured the same
day against the Deck (SteamOS **3.8.15**, build 20260715.1, board **Galileo/OLED**):

| package | Deck | archlinux:latest |
|---|---|---|
| glibc | 2.41 | 2.43 |
| **gcc-libs** | **15.1.1** | **16.1.1** — a full major ahead |
| **systemd-libs** | **257.7** | **261.1** — sd-bus is the updater's transport |
| curl | 8.15.0 | 8.21.0 |
| **flatpak** | **1.16.6** | **1.18.0** — the portal IS the self-update foundation |
| libevdev | 1.13.4 | 1.13.6 |
| libxcb | 1.17.0-1 | 1.17.0-1 (the only exact match) |

So `ok - launcher builds + L0 green on Arch` was an honest sentence answering a **different question**
than a reader takes from it: it proved the launcher builds against *tomorrow's* glibc/gcc/libsystemd,
not the Deck's, compiling with **GCC 16** — neither of the two toolchains doc §13.2 pins (Clang >=18 /
GCC 14) nor the one SteamOS ships. Same family as `just power` reporting `mean 0.00 W ... PASS` off a
missing battery node: a check aimed slightly past its target. Note especially the **portal suite**,
the whole foundation of self-update, was validating against a flatpak the Deck does not run.

**Fixed** by `docker/steamos.Dockerfile`: a two-stage build that bootstraps a pure SteamOS rootfs from
Valve's real mirror (archlinux is used only to run `pacman` and nothing from it survives into the
image). Every installed package now resolves to the **exact version string the Deck reports** —
verified: glibc `2.41+r65+ge7c419a29575-1`, gcc `15.1.1+r7+gf36ec88aa85a-1`, systemd-libs `257.7-2.5`,
curl `8.15.0-1`, libpulse `17.0+r43+g3e2bb8a1e-1`, libxcb `1.17.0-1`, libevdev `1.13.4-1`, flatpak
`1:1.16.6-1.1`, bubblewrap `0.11.0-1`. The launcher builds and all 21 L0 tests pass on it.

**The repos were READ OFF THE DEVICE, never guessed.** `scripts/sim/capture-deck-baseline.sh` pulls
the unit's `/etc/os-release`, package versions and its own `/etc/pacman.conf` + mirrorlist into
`docker/steamos-baseline.env`. The branch is **`3.8.1x`** (`jupiter-3.8.1x`, `holo-3.8.1x`,
`core-3.8.1x`, …) off `https://steamdeck-packages.steamos.cloud/archlinux-mirror/$repo/os/$arch`.
Guessing would have produced "holo-3.5" and been silently wrong — this is the no-guessing rule the
keymap work already refuses to break, applied to the sim.

**On the pin.** The gcc/glibc pin exists for REPRODUCIBILITY, and it is worth being exact about what
it does not mean: the Deck never compiles anything. The shipped launcher is built by the **Flatpak
SDK's GCC** (`org.freedesktop.Platform//25.08`, GCC 14) and runs against that runtime's libs, so
SteamOS's compiler is **not** the release toolchain and must never be described as one. What the pin
buys: archlinux:latest silently swapped the compiler between runs, so a green sim result was not
reproducible and its toolchain was nobody's. The Dockerfile now **fails the build** on drift
(`PIN DRIFT: gcc is X, pinned to Y`) rather than drifting quietly — verified with a bogus pin.

### 2. `reconnect` was green only on a permissive host, and the requirement was never stated

`just sim reconnect` failed here — `bwrap: Failed to make / slave: Permission denied` — tripping the
positive control ("monitor never came up"). Verified **on `main`**, so not a regression: **this suite
had never run on a native Linux host at all.** Confirmed cause: `kernel.apparmor_restrict_unprivileged_userns = 1`
(Ubuntu >= 23.10), which WSL2 does not set. `run.sh` passed only `--security-opt seccomp=unconfined`.

Measured minimum, each one load-bearing — `--privileged` buys **nothing** over this set:

| flag | without it |
|---|---|
| `--security-opt seccomp=unconfined` | docker's default seccomp blocks `unshare` |
| `--security-opt apparmor=unconfined` | `bwrap: Failed to make / slave: Permission denied` |
| `--cap-add SYS_ADMIN` | `bwrap: setting up uid map: Permission denied` |

With all three, all four suites are green here — **including `reconnect`, which now genuinely passes
on this host for the first time.**

The exit code was wrong in the same breath: bwrap being denied a privilege is an ENVIRONMENT failure
(**3**) and the suite reported **2 = the product is wrong** — the precise category error HARNESS §1
exists to prevent, committed by the harness itself. A reader trusting the taxonomy would hunt a
reconnect bug that isn't there. (Still open: `incontainer.sh` maps every `bad()` to exit 2. The
bwrap-privilege case specifically should be a 3.)

### What none of this changes

A SteamOS-userspace container still has **no GPU, no VA-API, no gamescope, no battery, no ACPI**. The
exit-6 refusal below is untouched. Matching userspace makes the **build** and **D-Bus** layers
faithful and widens **nothing** else. `tests/harness/test_sim_guardrail.sh` now pins all of it (14
checks): the hardware refusals, that `run.sh` builds the SteamOS image, that `incontainer.sh` asserts
`DECKBACK_SIM_BASE=steamos` before running anything (a wrong image exits 3, verified), that all three
bwrap flags are granted, and that the toolchain pin fails the build on drift.

### For the next SteamOS update

Re-run `scripts/sim/capture-deck-baseline.sh` against a Deck. **The git diff of
`docker/steamos-baseline.env` IS the upgrade note** — it names exactly what moved under the launcher.
Bump `DECKBACK_STEAMOS_BRANCH` / `DECKBACK_STEAMOS_GCC` / `DECKBACK_STEAMOS_GLIBC` in
`docker/steamos.Dockerfile` to match, then re-run `just sim all` before trusting any prior green. The
build refuses to proceed on an unbumped pin, so this cannot be forgotten silently. The baseline
records **one unit** — Galileo/OLED; an LCD (Jupiter) baseline would legitimately differ and has
never been captured (durable/hardware.md).

## Status: Phase 2 (reconnect drive) LANDED 2026-07-16 — `just sim` runs four GPU-independent suites green on a real SteamOS 3.8.1x userspace

Landed and green (`just sim`, `docker/sim.Dockerfile` + `scripts/sim/run.sh` + `incontainer.sh`):
- **`launcher`** — the out-of-tree launcher builds + all 21 L0 tests pass on **Arch** (not just the
  repo's Debian toolchain), proving it isn't distro-locked. Arch's `systemd-libs` ships `sd-bus.h` +
  `libsystemd.pc`, so this build is **with** sd-bus — the real `PortalUpdater`, not the stub.
- **`shortcut`** — the installer's Steam-tile writing (`steam_shortcuts.py add` + `art`) produces a
  correct `shortcuts.vdf` entry + the five `grid/<crc-appid>*.png` files against a synthetic Steam
  userdata layout. This is the "prove the installer with steamlauncher" ask — faithful, no Deck.
- **`portal`** — `flatpak-portal` + the PermissionStore activate and enforce their caller contract
  (the self-update / reconnect foundation).
- **`reconnect`** (Phase 2, 2026-07-16) — drives the launcher's real D-Bus reconnect logic
  (`durable/dbus-reconnect.md`) off hardware. A bounded `deckback-launcher --selftest-watch <secs>`
  — implemented in its own translation unit `launcher/src/simwatch.{cpp,hpp}`, **isolated** from the
  shipping `updater.cpp` (it uses only the public `Updater` start()/stop() interface), so the
  simulator surface doesn't bleed into the self-update code — runs the actual `PortalUpdater` loop
  **inside a bwrap flatpak-instance** (synthetic
  `/.flatpak-info` → the portal accepts `CreateUpdateMonitor`; a self-managed `dbus-daemon` at a
  FIXED socket path so the bus can be dropped and restored at the same address). Four asserts, all
  green and stable across 3 runs: **positive control** (monitor comes up — the flatpak-instance
  caller is accepted; a non-flatpak caller is what `portal` rejects), **case B** (`pkill
  flatpak-portal` + reactivate → `NameOwnerChanged` → "flatpak-portal restarted — re-creating the
  update monitor"), **case A drop** (`kill` the daemon → "session bus error — attempting to
  reconnect"), **case A recover** (restart the daemon at the same address → "reconnected to the
  Flatpak portal"). It refuses to pass a libsystemd-less (stub) build. **This retires the on-Deck
  reconnect drive** — the drive that a sleeping Deck kept blocking now runs in CI-reachable software.
- **Guardrail** — `just sim vaapi|power|soak|resume|…` exits **6 (UNSUPPORTED-IN-SIM)**, pinned by
  `tests/harness/test_sim_guardrail.sh` so the honesty rule can't rot.

Still ahead: the full self-update **deploy** round-trip (a real staging-repo commit to Update() to
Done — needs an ostree repo fixture), and the `sim-gamescope` overlay-render + uinput tier (Phase 2b
below).

## Why, and the one rule that makes it safe

Requested: run SteamOS in Docker to simulate the Deck for full-automation tests instead of the real
device. Research verdict (durable): **a container faithfully runs some layers and CANNOT run the
ones that actually gate a release.** SteamOS/Holo container images exist and gamescope has a
`--backend headless` on software Vulkan (lavapipe), but:

- **VA-API hardware decode** (P4: `VaapiVideoDecoder`, the green-band pixel verdict, 5.6 W) needs the
  Van Gogh/Sephiroth **APU** + radeonsi + ANGLE's DMA-buf import. lavapipe is software and has **no
  VA-API**. This is the exact path that **false-passed every automated metric on m114** — a human saw
  the corruption the metrics missed. This host has **no `/dev/dri` at all**, so the point is moot here.
- **Power** (no battery → the `0.00 W … PASS` trap), **suspend/resume** (no ACPI/`rtcwake`), the real
  **Deck APU** (LCD/OLED), and true **Game-Mode/Steam-session** fidelity are equally out of reach.

So the sim is built under **one hard, architectural rule**: it is **incapable of emitting PASS for a
GPU / VA-API / power / resume gate.** Those tests `skip` with a loud notice (a dedicated exit code),
never green. A sim that could green a hardware gate would manufacture exactly the false confidence
this project is scarred by (TEST-PLAN §0). Everything the sim *can* run faithfully, it runs for real.

## Tiers (slots between L1 `smoke` and L2 on-Deck)

- **L1.5 `sim-portal`** — no GPU, no compositor. A session bus + `flatpak-portal` + a staging
  archive-z2 ostree repo. Automates, deterministically, in CI:
  - self-update **detect → notify → deploy** (the "T1 staging" flow, today hand-run on a Deck),
  - **D-Bus reconnect case A** (drop the bus) and **case B** (`restart flatpak-portal`) → assert the
    monitor rebuilds (`durable/dbus-reconnect.md` — currently Deck-blocked; this unblocks it),
  - permission-store seed, `flatpak install/update`, and `steam_shortcuts.py add` (vdf, already L0).
  - Uses the launcher's `--selftest-update` / `--selftest-deploy[-seed]` + a new bounded
    `--selftest-watch` that runs the real updater loop so an external portal restart / bus drop is
    observable.
- **L1.7 `sim-gamescope`** (stretch) — `gamescope --backend headless` on lavapipe + a session bus.
  Runs the launcher + `content_shell` to test what is **compositor/DOM plumbing, not GPU**: overlay
  injection under a real Wayland compositor (the CSP/Trusted-Types + `adoptedStyleSheets` path,
  capture⇔paint, Leanback body-swap survival), and evdev input via **uinput**. Every render result is
  stamped **"software render — NOT the Deck GPU path"** so it can never be read as hardware proof.

## What each tier explicitly does NOT cover (skip-loud, never PASS)

VA-API/hardware decode, the green-band/pixel verdict, frame-drop counts, power draw, suspend/resume,
EPP, real Deck-APU/LCD-vs-OLED behavior, true Game-Mode Steam-session semantics, touchscreen
EVIOCGRAB-vs-gamescope. These remain **Deck-only** (L2), and the sim's runner prints that verbatim.

## Architecture

- **Base:** Arch (honors "SteamOS is Arch-based"); the portal/flatpak layer is distro-agnostic. The
  gamescope layer needs Arch/AUR gamescope + mesa lavapipe. (The repo's *build* toolchain stays
  Debian-12; the sim is a separate runtime image, not a build image.)
- **`sim-portal` image:** `flatpak`, `dbus`, `ostree`, `glib2` (gdbus), python3. Entry: start a
  session bus, publish commit A→B into a served staging repo, install A `--user`, run the launcher
  selftests, and script portal restart / bus drop for the reconnect asserts.
- **`sim-gamescope` image:** the above + `gamescope`, `mesa`/`vulkan-swrast` (lavapipe),
  `xorg-server-xvfb` or wayland, `content_shell`. Needs unprivileged userns for bwrap/zypak
  (`--security-opt seccomp=unconfined`, or a documented cap set) — validated in the spike.
- **Recipes:** `just sim [portal|gamescope]` → `scripts/sim/*.sh`. Exit-code taxonomy reuses
  HARNESS §1 with a new **`6 = UNSUPPORTED-IN-SIM`** (a hardware-only gate was asked of the sim) so a
  skipped hardware gate is never silently a 0.
- **L0 for the harness itself:** the sim's pass/fail logic (what it refuses to green) gets
  `tests/harness/` coverage — a check that cannot fail is not a check (HARNESS §0).

## Phased plan

1. **Phase 1 — `sim-portal`** (feasibility spiked first: does `flatpak-portal` answer in a
   container?). Deliver the self-update + reconnect automation. Highest value, fully faithful,
   unblocks the Deck-pending reconnect drive.
2. **Phase 2 — `sim-gamescope`** overlay-render + input tiers, guardrailed as software-only.
3. **Docs** — TEST-PLAN.md gains the L1.5/L1.7 rows with the honest coverage boundary; HARNESS.md
   gains the recipes + the `6` exit code; this finding is the rationale.

## Feasibility — PROVEN for Phase 1 (2026-07-15, spiked in Docker on this WSL2 host)

Reproducible probe: `scripts/sim/portal-probe.sh` (Arch container, `--security-opt seccomp=unconfined`).

- ✅ **`flatpak-portal` runs and answers in a plain container.** A `dbus-run-session` activated both
  `org.freedesktop.portal.Flatpak` AND `org.freedesktop.impl.portal.PermissionStore` (flatpak 1.18.0,
  Arch) — the two services the updater's seed + monitor talk to. No SteamOS image needed.
- ✅ **The portal's caller contract is a clean negative→positive control.** From a non-flatpak caller
  `CreateUpdateMonitor` returns `NotSupported: "Updates only supported by flatpak apps"`. So the sim's
  test caller must present `/.flatpak-info` — i.e. run as a real flatpak instance. The faithful way is
  `flatpak run` of a minimal app (flatpak sets `/.flatpak-info` up correctly); a hand-rolled `bwrap`
  wrapper with a synthetic `/.flatpak-info` at a fresh tmpfs root works too.
- ✅ **`bwrap` userns works** under `docker run --security-opt seccomp=unconfined` (default seccomp
  blocks the `unshare`; unconfined allows it). So flatpak install/run inside the container is viable.
- ✅ **No `/dev/dri` on this host** — confirming the sim is software-only here and the GPU guardrail is
  mandatory, not optional.

Net: Phase 1 (`sim-portal`) is buildable now. Concrete next steps — (1) `docker/sim.Dockerfile`
(Arch + flatpak/ostree/dbus + a minimal runtime); (2) a tiny real flatpak app that wraps
`deckback-launcher` so the portal accepts it; (3) a staging archive-z2 repo helper (publish A→B); (4)
a bounded `deckback-launcher --selftest-watch <secs>` mode that runs the real updater loop so a
scripted `restart flatpak-portal` (case B) / bus drop (case A) is observable; (5) `just sim portal`
+ the `6 = UNSUPPORTED-IN-SIM` exit code + the L0 guardrail test.

## OPEN / risks

- flatpak + bwrap in Docker needs unprivileged userns; portal `Update` (deploy) may need
  `--privileged`/cap tuning — the spike settles the minimum.
- gamescope-headless on lavapipe stability in a container (issue trail is mixed) — Phase 2 gate.
- Keeping the guardrail honest as tests grow: the runner must default-deny any result label in the
  hardware set, enforced by an L0 test, not developer discipline.
