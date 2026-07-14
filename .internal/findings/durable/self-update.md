# Self-update via the Flatpak portal (launcher/src/updater.cpp)

**Status:** IMPLEMENTED 2026-07-13, **off by default**, **NOT yet verified on a Deck.** First shipped
in the release that carries `updater.cpp` (targeted 0.0.4). "Implemented" is not "verified" — the
portal path only runs inside the flatpak on real hardware.

## Why this shape

The launcher runs **inside the Flatpak sandbox**, so it cannot run `flatpak update`. The sanctioned
path for a sandboxed app to update itself is the **Flatpak portal** (`org.freedesktop.portal.Flatpak`),
a privileged host service that performs the update on the app's behalf. It updates **only this app's
own ref, from this app's own origin remote** (our `--user` `deckback` remote) — no root, and for a
`--user` install **no polkit/password prompt** (which Game Mode could not answer). It touches nothing
else the user installed: blast radius = Deckback. (A full `flatpak update` — the systemd timer or
Discover — is the separate tool that updates the runtime and every other app; see the update-parity
discussion in `docs/`/`RELEASING.md`.)

The update deploys a **new ostree commit without swapping the running deployment**, so playback is
never interrupted; the new version binds on the **next launch**. On completion the launcher toasts
"restart to apply" (reusing the CDP overlay, `config/scripts/toast.js`) if a CDP port is available.

## Architecture

`updater.cpp` mirrors `platform.cpp` exactly, but on the **session** bus (`sd_bus_open_user`) instead
of the system bus — one owned event-loop thread, sd-bus only, a stub when `<systemd/sd-bus.h>` is
absent. No new dependency (libsystemd is already linked; the toast reuses DevToolsClient/overlay).

Portal interface (from `data/org.freedesktop.portal.Flatpak.xml`, pinned here so nobody re-derives it):
- `org.freedesktop.portal.Flatpak` at `/org/freedesktop/portal/Flatpak`:
  `CreateUpdateMonitor(a{sv} options) → o handle`
- `org.freedesktop.portal.Flatpak.UpdateMonitor` (on the returned handle path):
  - `Update(s parent_window, a{sv} options)` — we pass `""` + empty dict
  - `Close()`
  - signal `UpdateAvailable(a{sv})` — keys `running-commit`, `local-commit`, `remote-commit`
  - signal `Progress(a{sv})` — keys `n_ops,op,progress,status,error,error_message`;
    **status: 0 Running · 1 Empty (nothing to do) · 2 Done · 3 Failed**

Flow: create monitor → on `UpdateAvailable` set a flag → the loop (never the signal callback, to
avoid a reentrant `sd_bus_call` inside `sd_bus_process`) calls `Update` → on `Progress` status 2,
toast + mark ready. The two sync calls carry a 5 s timeout so a missing portal fails fast on the
background thread instead of sd-bus's 25 s default.

Note: the portal compares **ostree commits, not version strings**. "An update is available" means the
installed ref's remote has a newer commit on its branch — version labels (0.0.3 vs 0.0.4) are
cosmetic to the portal.

## Config & permission

- `app.json: self_update` (bool, **default false**), hot-swappable like every other flag. Off = the
  `Updater` is never constructed; the feature is fully inert.
- Manifest: `--talk-name=org.freedesktop.portal.Flatpak` (session bus). Inert while the feature is off.
  Flathub is picky about this name — note it for the eventual Flathub submission; we are self-hosted.
- `--version` and startup now log the real version, compiled in from the repo `VERSION` file
  (`version.hpp`, CMake `configure_file`; the manifest adds `VERSION` as a flat `file` source).

## Verification — REQUIRED before this is trusted (nothing below has run yet)

Tiered, cheap → expensive:

- **T0 (workstation): `deckback-launcher --selftest-update`** inside the flatpak — creates a monitor
  and confirms the portal answered. Proves the D-Bus plumbing/types. Off-Deck it correctly reports
  "no live session bus"/portal-unreachable and exits 1.
- **T1 (Deck, staging repo): the authoritative functional test.** Install commit A (with the updater)
  on the Deck from a **local/staging ostree repo** (`just publish-repo` + `python3 -m http.server`,
  added as a `--user` remote — see RELEASING.md "Test the repo locally"). Publish a newer commit B to
  the same repo. Launch A, confirm: `UpdateAvailable` fires, `Update` deploys B, the toast shows, and
  the **next launch runs B** — with **no password prompt**. This proves detect + apply + custom
  remote + restart-to-apply without touching production.
- **T2 (Deck, production, at release): the real-URL round-trip.** The official repo URL already
  delivers updates (that is how 0.0.2→0.0.3 happened via `flatpak update`); the portal pulls from the
  same remote. So T1 proves the new behaviour; T2 just confirms the production URL, cheaply, as a
  0.0.4→0.0.4.1 (or 0.0.4→0.0.5) republish once 0.0.4 is live.

**Chicken-and-egg (state it openly):** the updater ships FIRST in 0.0.4, so users already on 0.0.3
(no updater) must update once manually to get it; auto-update works from 0.0.4 onward. The first hop
is always manual when introducing a self-updater — that is expected, not a bug.

## Open questions (until the Deck runs it)

- Is `org.freedesktop.portal.Flatpak` reachable in the gamescope session as user `deck`? (It is
  D-Bus-activatable; unconfirmed on SteamOS Game Mode.)
- Does `Update()` on a `--user` custom-remote ref ever raise a polkit prompt? (Expected: no.)
- LCD (Van Gogh) unit: entirely untested, as with everything else.
- Verify the `Progress.status` numeric codes against the installed flatpak version on the Deck (they
  are stable in the XML, but this feature's whole correctness hinges on `2 == Done`).
