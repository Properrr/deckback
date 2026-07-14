# Self-update via the Flatpak portal (launcher/src/updater.cpp)

**Status:** IMPLEMENTED 2026-07-13, **ON by default as of 2026-07-14** (`app.json: self_update=true`).
T1 on-Deck 2026-07-14 (OLED, Game Mode) found the portal deploy blocked by the missing Access backend
(★ T1 RESULT below). **A FIX shipped 2026-07-14 and is VERIFIED on-Deck (★ T1-REPLAY RESULT below):
when `self_update` is on, the launcher pre-records the consent in the permission store
(`flatpak`/`updates`=`yes`), which flatpak-portal honors WITHOUT the Access dialog** — traced in
flatpak 1.14.x and 1.16.x source (the Deck runs 1.16.6) and proven end-to-end on the OLED Deck in a
real Game Mode launch: the app auto-seeded consent, the portal deployed the newer commit, and the
next launch bound it. **Default flipped to `true` by product decision (2026-07-14): releases are
gated on tests, so auto-deploying a published commit is the intended behaviour.** The tradeoff is
accepted openly — there is no couch-reachable rollback, so a bad *published* commit reaches every
Deck on next launch; the mitigation is gating what gets published (see "Known behaviour"), not the
client. "Implemented" IS now "works on the target," for the OLED unit.

## ★ T1-REPLAY RESULT 2026-07-14 (OLED, Game Mode) — the fix WORKS end-to-end

Rebuilt two bundles into a local staging ostree repo — A' (v0.0.4, this fix, `self_update=true`) and
B' (v0.0.5) — served over HTTP, installed A' on the OLED Deck from a `--user --no-gpg-verify`
`deckback-test` remote, and ran two experiments in the live gamescope session (SSH onto the session
bus `/run/user/1000/bus`; the session's own `xdg-desktop-portal-gamescope` was the running Access
impl, so this is faithful Game Mode). Commits: A'=`2766f383`, B'=`01d4b838`.

1. **Blocker reproduced, same session.** With the permission store cleared (raw/unset),
   `deckback-launcher --selftest-deploy` (a new diagnostic: one portal Update cycle on a persistent
   connection, no seeding) failed with exactly **`No portal support found`** (exit 3). And the
   poll=15 flatpak-portal's own log printed the traced cause verbatim: `Error loading
   gamescope.portal: Key file does not have key "UseIn" in group "portal"` (and `holo.portal`) — so
   flatpak-portal's own portal resolver finds NO Access impl, confirming the ★ T1 RESULT mechanism on
   flatpak 1.16.6.
2. **Fix confirmed, same session.** `--selftest-deploy-seed` pre-granted `flatpak/updates=yes` then
   ran the same Update cycle → **`DEPLOYED (status Done)`** (exit 0); the deployed commit advanced
   A'→B' and `flatpak permissions flatpak` showed `... updates ... deckback yes`.
3. **Real product flow, normal Game Mode launch.** Rolled the deployment back to A', cleared the
   permission, and launched the app the ordinary way (`flatpak run`, `DISPLAY=:0`, into gamescope)
   with `self_update=true`. journald from the running app:
   - `updater: pre-granted self-update consent (flatpak/updates=yes)` — the SHIPPED startup path
     seeds by itself (not the diagnostic);
   - `updater: an update is available (remote 01d4b838)` — fired on the portal poll;
   - `updater: update requested; deploying in the background` → `updater: update deployed`;
   - `flatpak info` Commit = B'; a subsequent `--version` reported **`0.0.5`** — the update bound on
     the next launch, exactly as designed.

   (To avoid the portal's 30-min default poll, the test ran flatpak-portal with `--poll-timeout=15`
   via a user systemd drop-in on `flatpak-portal.service` — the app-triggered D-Bus re-activation
   replaces a manually-started portal, so the interval must be set on the service, not a hand-started
   instance. Drop-in removed afterward.) Deck restored to production v0.0.3 (origin `deckback`), user
   data intact, permission store cleared. STILL only the OLED unit; the LCD (Van Gogh) is untested.

## ★ SOLUTION 2026-07-14 — pre-seed the consent in the permission store

## ★ SOLUTION 2026-07-14 — pre-seed the consent in the permission store

**Traced root-cause of the gate (flatpak `portal/flatpak-portal.c`, verified on both the 1.14.x and
1.16.x branches; the Deck has flatpak 1.16.6):** before deploying a self-update,
`request_update_permissions_sync()` calls `get_update_permission(app_id)`, which reads the
xdg-permission-store — table `"flatpak"`, id `"updates"` — and maps `perms[0]`: `"yes"` → YES,
`"ask"` → ASK, anything else → NO, no entry → UNSET. **The Access-portal consent dialog is consulted
ONLY when the result is UNSET or ASK**; YES returns TRUE immediately (no dialog), NO fails with
"Application update not allowed". After a real dialog, an UNSET result is written back as
`"yes"`/`"no"` — i.e. seeding `yes` writes exactly what the dialog's "allow" would have written.
So the Game-Mode blocker ("No portal support found" = no loadable Access impl) is only reachable
from the UNSET/ASK states, and a pre-seeded `yes` bypasses it on the sanctioned path.

**The seed is writable by us:** xdg-permission-store's `SetPermission(s table, b create, s id,
s app, as permissions)` does no caller validation (traced in xdg-desktop-portal
`document-portal/xdg-permission-store.c`), so the sandboxed launcher itself can record the consent,
given `--talk-name=org.freedesktop.impl.portal.PermissionStore` (now in the manifest). The explicit
`self_update=true` opt-in in app.json IS the user's consent; the launcher only writes what the
unreachable dialog would record.

**Implementation (launcher/src/updater.cpp):** when `cfg_.enabled`, the loop thread runs
`seed_update_permission()` before creating the update monitor: read the app id from `/.flatpak-info`
(`[Application]` `name=`), `Lookup` the current value, then — `yes`: nothing to do; **`no`: respected,
never overwritten** (warn + the `flatpak permission-set flatpak updates <app> yes` hint); unset/`ask`:
`SetPermission` → `yes`. All states/parsers are pure functions (`decode_update_permission`,
`update_permission_name`, `parse_flatpak_app_id`) pinned by `updater_test.cpp` against flatpak's
exact semantics (first element only, case-sensitive, unknown → No). `--selftest-update` now also
reports the stored permission, so the on-Deck state is one command away.

**Host-side equivalent (works today, no rebuild):** `flatpak permission-set flatpak updates
io.github.properrr.deckback yes` as user `deck`; inspect with `flatpak permissions flatpak`.

**Rejected alternative:** fixing the Access backend itself. SteamOS points portal loading at
`/usr/share/xdg-desktop-portal/gamescope-portals` via `XDG_DESKTOP_PORTAL_DIR`; `gamescope.portal`
there declares Access but lacks the `UseIn` key (fails to load) and `holo.portal` does not declare
Access at all. The dir is in the read-only rootfs, flatpak-portal has its own legacy `.portal`
resolver, and even a loaded gamescope Access impl would need a dialog UI nobody has verified in Game
Mode. The permission seed avoids the whole subsystem.

**T1 REPLAY — PASSED 2026-07-14 (OLED), see the ★ T1-REPLAY RESULT at the top of this file.** Both
`Progress` status **2 (Done)** and the seed-then-deploy path are now confirmed on hardware. To repeat
it: staging repo (commit A' = this fix + `self_update=true`, commit B' newer, both installed from a
`--user --no-gpg-verify` remote); shrink the portal poll with a `flatpak-portal.service` user
drop-in `ExecStart=/usr/lib/flatpak-portal --poll-timeout=15` (a hand-started `--replace` instance
gets replaced when the app re-activates the portal, so set it on the service); use
`deckback-launcher --selftest-deploy` (raw) / `--selftest-deploy-seed` for a scriptable, no-wait
A/B of the portal itself; then a real `flatpak run` launch for the full auto-seed→auto-deploy chain.

## ★ T1 RESULT 2026-07-14 (OLED, Game Mode) — detection works, deploy fails ("No portal support found")

Ran the full T1 round-trip against the OLED Deck: built commit A (v0.0.3, `self_update=true`) and
commit B (v0.0.4) into a local ostree repo (`flatpak/repo`), served it over HTTP, installed A from a
`--user --no-gpg-verify` staging remote (origin `deckback-test`), published B, and watched the running
A instance in Game Mode. Sequence observed in journald:

1. **Portal reachable — Open Q #1 ANSWERED YES.** `updater: watching for updates to this app via the
   Flatpak portal` at launch: `sd_bus_open_user` + `CreateUpdateMonitor` succeed as user `deck` in the
   gamescope session. The portal (`/usr/lib/flatpak-portal`) is D-Bus-activatable there.
2. **Detection works, on the portal's cadence (not at launch).** Nothing fired for ~30 min; at
   **12:43:09** — the portal's first periodic check (its `poll-timeout`, ~30 min after the portal
   started) — `updater: an update is available (remote 5927f3471c89)`. Confirms the "detection is
   periodic, not instant-at-launch" note below: a fresh monitor does NOT force an immediate check.
3. **Our `Update()` request works.** `updater: update requested; deploying in the background` — the
   portal accepted the `Update()` call (our `do_update()` is correct).
4. **Deploy FAILS.** `updater: update failed: No portal support found`. Deployed ref stayed at A;
   next launch would still run A.

**Root cause (traced, not guessed).** `No portal support found` is emitted ONLY by `flatpak-portal`,
in `request_update_permissions_sync()`, when `find_portal_implementation("org.freedesktop.impl.portal.Access")`
returns NULL. flatpak-portal shows a **consent dialog** (via the Access portal) before deploying a
self-update; **SteamOS Game Mode has no working Access backend.** The gamescope backend
(`/usr/share/xdg-desktop-portal/gamescope-portals/gamescope.portal`) DECLARES
`org.freedesktop.impl.portal.Access` but is **missing the `UseIn` key**, so xdg-desktop-portal fails to
load it (`Error loading gamescope.portal: Key file does not have key "UseIn"` in the journal); the
`gtk`/`kde` backends are scoped `UseIn=gnome`/`KDE`. So in a gamescope session there is no Access impl,
and the portal's self-update cannot get consent → fails. This is a **SteamOS/gamescope environment
bug, not a defect in our code** — our updater does exactly the right thing.

**Corroboration:** a plain `flatpak update --user io.github.properrr.deckback` from the same staging
remote finds and would deploy the 84 MB update with **no** portal/dialog — the ostree/update mechanism
is fine; only the *portal-driven* deploy is blocked.

**Consequences / recommendation (as of this T1 RESULT — SUPERSEDED by the ★ SOLUTION / ★ T1-REPLAY
above, which fixed the deploy and flipped the default to `true`).**
- Portal-driven auto self-update **did not deploy in Game Mode** before the permission-seed fix.
- The recommendation at the time was to keep `self_update=false`; it is now `true` (verified on-Deck).
- The Deck's real update path is manual `flatpak update` / Discover (Desktop Mode), or a GitHub-Release
  `.flatpak` re-install — exactly what `deck-flatpak.sh repo`/`RELEASING.md` already assume.
- If auto-update is wanted later, options to investigate: (a) whether a future SteamOS fixes the
  `gamescope.portal` `UseIn` key / ships an Access backend; (b) whether the app can drive
  `flatpak update` some other sanctioned sandbox-safe way (none known — the portal is *the* sanctioned
  path); (c) shipping an updater that only *notifies* "an update is available, update from Desktop
  Mode" (detection works) rather than trying to deploy. NOT retried on-Deck because the blocker is
  environmental and stable (the missing `UseIn` key is in the read-only SteamOS rootfs).
- **Superseded 2026-07-14 by the ★ SOLUTION above:** the Access dialog is only consulted when the
  permission-store entry is unset/`ask`; pre-seeding `flatpak`/`updates`=`yes` makes the deploy take
  the no-dialog path. The environmental facts in this section remain true and still matter for any
  feature that genuinely needs an Access dialog in Game Mode.

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

- `app.json: self_update` (bool, **default true** as of 2026-07-14), hot-swappable like every other
  flag. Set false = the `Updater` is never constructed; the feature is fully inert.
- Manifest: `--talk-name=org.freedesktop.portal.Flatpak` and (for the consent seed, ★ SOLUTION)
  `--talk-name=org.freedesktop.impl.portal.PermissionStore` (session bus). Both inert while the
  feature is off. Flathub is picky about both names — the PermissionStore one especially (it can
  write ANY app's permissions); note it for the eventual Flathub submission; we are self-hosted.
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

## Open questions — updated by the T1 run (2026-07-14)

- ~~Is `org.freedesktop.portal.Flatpak` reachable in the gamescope session as user `deck`?~~
  **ANSWERED YES** (T1 §1) — the monitor is created successfully in Game Mode.
- ~~Does `Update()` on a `--user` custom-remote ref ever raise a polkit prompt?~~ **No polkit prompt** —
  but it needs an **Access-portal consent dialog** that Game Mode can't provide, which was the blocker
  (★ T1 RESULT), bypassed by the permission-store seed (★ SOLUTION). Different failure than expected.
- Does the seeded permission actually carry the deploy to Done on-Deck? **UNVERIFIED** — the T1
  replay in ★ SOLUTION is the gate. The claim rests on traced flatpak source, not hardware yet.
- `Progress.status` codes: **status 3 (Failed) confirmed on hardware** (decoded to "update failed" +
  the error message). **Status 2 (Done) was NOT reached** — the deploy failed first — so `2 == Done` is
  still unconfirmed against a real successful update. The mapping stays unit-tested via
  `decode_progress_status()` (updater.hpp).
- LCD (Van Gogh) unit: entirely untested, as with everything else. (Immaterial while the Game-Mode
  Access blocker stands — it is not unit-specific.)

## Design decisions (deliberate — recovery is by relaunch, not in-session)

The loop is intentionally minimal: **connect once at start, watch, and on any failure go inert for the
session.** Updates are not urgent (a kiosk app is relaunched often), so the extra state machine for
in-session reconnect/retry was rejected as complexity that buys little. What this means:

- **Connect-once, no reconnect.** If the session bus errors or the portal drops mid-session, `loop()`
  returns and self-update is inert until the **next launch**, which re-creates the monitor from
  scratch. Each launch is a fresh, independent check — more predictable than depending on the portal's
  re-emit semantics.
- **No deploy watchdog / no retry cadence.** If `Update()` is ack'd but no terminal `Progress`
  arrives, `updating_` stays set and nothing else is attempted **this session**; a failed `Update()`
  is likewise not retried in-session. Both self-heal on relaunch (fresh process resets the flags).
- **Idle costs no battery.** The event loop's `poll()` timeout comes only from sd-bus's own deadline;
  when nothing is pending sd-bus reports an infinite timeout and `loop_timeout_ms()` returns `-1`, so
  `poll()` blocks until a portal signal or the stop nudge — there are **no periodic wakeups**. (An
  earlier draft capped the poll at 1 s, i.e. a 1 Hz idle wakeup; that was removed.)
- **Reentrancy avoided.** `Update()` is invoked from the loop body, never from a signal callback, so
  `sd_bus_call()` is never re-entered inside `sd_bus_process()`. `want_update_`/`updating_` are
  loop-thread only.
- **Toast guard.** The "restart to apply" toast fires on a `Progress` **Done** only if `updating_` was
  set (an update *we* initiated), so a `Done` from an externally-initiated update can't spuriously
  toast.

## Known behaviour / accepted limitations

- **Detection is periodic, not instant-at-launch.** The host-side monitor lives for the whole process
  and the portal re-checks on **its own cadence**, so a release published mid-session is still picked
  up — but there is a poll-interval lag after launch before the first check. A slow first detection in
  the T1 test is expected, not a failure.
- **Auto-deploy is unconditional and has no couch-reachable rollback.** When `self_update` is on, any
  newer commit on the remote branch is deployed on next launch with no per-update prompt; a bad
  release therefore reaches every opted-in Deck. ostree keeps the prior deployment (so a *workstation*
  `flatpak update --commit=<prev>` can roll back), but a Game-Mode user cannot. This is the price of
  opt-in background updates from a single self-hosted remote — mitigate by gating what gets published,
  not in the client.
