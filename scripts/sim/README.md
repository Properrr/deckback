# `scripts/sim/` — containerized test sim (groundwork)

Design + rationale: [`.internal/findings/durable/test-sim.md`](../../.internal/findings/durable/test-sim.md).

A container can faithfully run the **GPU-independent** layers of Deckback's on-device tests
(self-update / Flatpak-portal / D-Bus reconnect, `flatpak` mechanics, and — later — overlay render on
software Vulkan + evdev input). It **cannot** run the gates that actually gate a release — VA-API
hardware decode, the green-band pixel verdict, power, suspend/resume, real Deck-APU behavior — and the
sim is built to be **incapable of reporting those as PASS** (they skip loud, exit code `6`, never
green). That guardrail is the whole reason a software sim isn't the m114 false-pass trap.

## Status

Design + **Phase-1 feasibility proven** (2026-07-15). Not yet the full `just sim` harness.

- `portal-probe.sh` — reproducible proof that `flatpak-portal` + the permission store run and enforce
  their caller contract inside a plain Arch container (no GPU, no SteamOS image). Run it:

  ```sh
  scripts/sim/portal-probe.sh
  ```

## Next (Phase 1 — `sim-portal`)

1. `docker/steamos.Dockerfile` — a REAL SteamOS 3.8.1x rootfs (Valve's holo/jupiter repos, pinned
   to the Deck's own gcc/glibc) + flatpak/ostree/dbus. Was archlinux:latest, which drifts ahead of
   SteamOS and quietly tested a toolchain nobody ships (durable/test-sim.md ★ CORRECTION).
2. A tiny flatpak app wrapping `deckback-launcher` so the portal accepts it as a flatpak instance.
3. A staging `archive-z2` repo helper (publish commit A → B) for the self-update round-trip.
4. `deckback-launcher --selftest-watch <secs>` — run the real updater loop bounded, so a scripted
   `restart flatpak-portal` (reconnect case B) / bus drop (case A) is observable and assertable.
5. `just sim portal` + the `6 = UNSUPPORTED-IN-SIM` exit code + an L0 test pinning the guardrail.
