# Containerized test sim (SteamOS/gamescope-in-Docker)

## Status: Phase 2 (reconnect drive) LANDED 2026-07-16 — `just sim` runs four GPU-independent suites green in Docker

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
  runs the actual `PortalUpdater` loop **inside a bwrap flatpak-instance** (synthetic
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
