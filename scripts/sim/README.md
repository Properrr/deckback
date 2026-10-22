# `scripts/sim/` — containerized test sim

Design + rationale: [`.internal/findings/durable/test-sim.md`](../../.internal/findings/durable/test-sim.md).

A container faithfully runs the **GPU-independent** layers of Deckback's on-device tests. It
**cannot** run the gates that actually gate a release — VA-API hardware decode, the green-band pixel
verdict, power, suspend/resume, real Deck-APU behavior — and the sim is built to be **incapable of
reporting those as PASS**: they are refused with exit code `6` (UNSUPPORTED-IN-SIM), never green.
`tests/harness/test_sim_guardrail.sh` pins that rule.

## Run it

```sh
just sim [suite]    # suite: all (default) | launcher | shortcut | portal | reconnect   (needs Docker)
```

| Suite | What it proves |
|---|---|
| `launcher` | the launcher builds and its L0 tests pass on the SteamOS userspace (the Deck's gcc/glibc), not just Debian |
| `shortcut` | the installer's Steam-tile writing (`steam_shortcuts.py add` + `art`) produces a correct `shortcuts.vdf` entry and grid art |
| `portal` | `flatpak-portal` + the permission store activate and enforce their caller contract |
| `reconnect` | the launcher's real D-Bus reconnect logic, via `deckback-launcher --selftest-watch <secs>` inside a bwrap Flatpak instance: portal restart, bus drop, bus recovery |

## Files

- `run.sh` — host side: refuses hardware gates (exit 6), builds the image from
  `docker/steamos.Dockerfile`, runs a suite against the read-only bind-mounted source.
- `incontainer.sh` — container side; do not run it on the host.
- `capture-deck-baseline.sh` — reads the real Deck's userspace (OS, glibc/gcc, pacman repos) into
  `docker/steamos-baseline.env`, which the image pins to. **Re-run it after every SteamOS update.**
- `portal-probe.sh` — standalone check that `flatpak-portal` answers inside a plain container.

## Not yet covered

- The full self-update **deploy** round-trip (a staging-repo commit through `Update()` to Done —
  needs an ostree repo fixture).
- Overlay render on software Vulkan + uinput input (`sim-gamescope`).
