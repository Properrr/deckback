# Self-update via the Flatpak portal (launcher/src/updater.cpp, updateprompt.cpp)

**Status in one paragraph.** The portal path is **verified on hardware**: the notify round-trip
(detect → Settings ▸ Updates → *Update now* → deploy → next launch runs the new version) passed
against the production remote on the **OLED** (0.0.5 → 0.0.6, 2026-07-15) and was recorded the same
day as confirmed on an **LCD** (Van Gogh) unit too — the only LCD result in the project, which owns
no LCD unit (`decisions.md`). It passed again on the OLED for 0.0.7 → 0.0.8 (2026-07-24, ordinary
Steam-shortcut launch). The consent seed, `Progress` status 2 (Done) and 3 (Failed), and the old
check-by-deploy answering Empty are all observed on-Deck. **Off-device only** (L0, TSan, CI replay,
harness): the ref probe, progress display, *Close now*, the stuck-deploy timeout, the stale-verdict
fixes, linear repo history with bsdiff deltas, the live permission gate, and the recall runbook —
T1 below is the pending gate; T2 passed with v0.0.10.

## Why the portal

The launcher runs **inside the Flatpak sandbox**, so it cannot run `flatpak update`. The sanctioned
path for a sandboxed app to update itself is the **Flatpak portal** (`org.freedesktop.portal.Flatpak`),
a privileged host service that updates **only this app's own ref, from this app's own origin remote**
(our `--user` `deckback` remote) — no root, and for a `--user` install **no polkit/password prompt**
(which Game Mode could not answer). Blast radius = Deckback. Updating the runtime and every other app
is a separate `flatpak update` (Desktop Mode / Discover).

The update deploys a **new ostree commit without swapping the running deployment**, so playback is
never interrupted; the new version binds on the **next launch**. The portal compares **ostree commits,
not version strings**: version labels are cosmetic to it.

## Portal interface (pinned so nobody re-derives it)

From `data/org.freedesktop.portal.Flatpak.xml`:
- `org.freedesktop.portal.Flatpak` at `/org/freedesktop/portal/Flatpak`:
  `CreateUpdateMonitor(a{sv} options) → o handle`
- `org.freedesktop.portal.Flatpak.UpdateMonitor` (on the returned handle path):
  - `Update(s parent_window, a{sv} options)` — we pass `""` + empty dict
  - `Close()`
  - signal `UpdateAvailable(a{sv})` — keys `running-commit`, `local-commit`, `remote-commit`
  - signal `Progress(a{sv})` — keys `n_ops,op,progress,status,error,error_message`;
    **status: 0 Running · 1 Empty (nothing to do) · 2 Done · 3 Failed**; `op` is 0-based.

`updater.cpp` mirrors `platform.cpp`'s shape on the **session** bus (`sd_bus_open_user`): one owned
event-loop thread, sd-bus only, a stub when `<systemd/sd-bus.h>` is absent. The two sync calls carry
a 5 s timeout so a missing portal fails fast instead of sd-bus's 25 s default.

## Modes (`self_update_mode`)

`notify` (default) | `auto` | `off`; the legacy boolean `self_update` still parses (`true`→`auto`,
`false`→`off`). `app.json` is read-only inside the Flatpak and `self_update_mode` is not in the
`user.json` allowlist, so **notify is what users run**; auto is not user-reachable today.

- **notify** — detection publishes availability into a shared `UpdateState` (atomic `available`,
  mutex-guarded remote commit) and waits. Deploy happens only on consent, via
  `Updater::request_update()`; the loop (never the caller) issues `Update()`. Verdicts go to the OSD
  Updates tab; notify never toasts.
- **auto** — deploys on detection; a completed deploy toasts "restart to apply"
  (`config/scripts/toast.js`). No per-update prompt and no couch-reachable rollback: a bad published
  commit reaches every auto Deck on next launch (ostree keeps the prior deployment, so
  `flatpak update --commit=<prev>` can roll back from a shell; a Game-Mode user cannot).
- **off** — the `Updater` is never constructed; fully inert.

The consent seed (below) runs in both notify and auto.

## Detection

**The ref probe (launcher, at launch and on *Check for updates*).** It GETs
`https://properrr.github.io/deckback/repo/refs/heads/app/io.github.properrr.deckback/x86_64/master`
(65 B, served with `max-age=600`) and compares it with `[Instance] app-commit` from `/.flatpak-info`:
equal means "latest", different means an update to confirm. **A check installs nothing.** The portal
remains the only installer and signature verifier, so a forged ref can at worst cause a harmless
Empty. `DECKBACK_UPDATE_REF_URL` points it at a staging repo. Guards, each with an L0 test that fails
without it:

- The probe only ever ADDS availability: a CDN-stale "up to date" never hides a portal announcement.
- A staged deploy (Done) is never re-offered.
- A commit the portal then answers Empty for is not re-announced by the probe for 10 min (the CDN's
  `max-age`, the likely cause).
- After a failed deploy, Check offers the same commit again — the retry the Failed text promises.
- An unreadable repo falls back to the portal check-by-deploy (the path verified on-Deck).
- No running commit (outside the sandbox) keeps the portal-only behaviour.
- Commits use the portal's 12-char short form (`short_commit`), so the ignore marker dedups across
  both sources.

**The portal's own `UpdateMonitor`.** flatpak-portal arms ONE global poll timer shared by every
app's monitor, so a fresh launch inherits whatever is left on it: first detection lands anywhere in
**0–30 min** (observed on-Deck: 60 s once, ~30 min once). It **never checks on a metered network**,
and the app is not told. The ref probe exists to close both gaps. After a successful deploy the portal
**re-announces the same commit** (the running instance still has the old one bound); the UI ignores it
because a deploy verdict takes precedence over availability. A manual Desktop-Mode `flatpak update`
binds on next launch and the portal then announces nothing.

## UI — OSD Settings ▸ Updates

`updateprompt.cpp::tick` feeds `OsdMenuController::set_update_state`, which draws the amber badge on
the Settings button and the **Updates** tab. The OSD is our own injected markup (`osd.js`), not
built on YouTube's elements, so a Leanback frontend change cannot break it.

- Actions: A on *Update now* (`confirm_update()`), Y / focused *Ignore* (`ignore_version()`),
  *Check for updates* (the ref probe), and *Close now* once a deploy is Done (the same graceful exit as
  the Exit hold).
- `updates_view()` is one pure function that derives status *and* buttons, so they cannot disagree.
  Terminal verdicts route `UpdaterConfig::on_terminal` → `UpdatePromptController::on_deploy_result` →
  `updates_view()`.
- Progress reaches the tab ("step 2 of 3 (45%)"). A deploy with no verdict times out as Failed after
  10 min, and every progress report restarts that timeout; a synchronously rejected `Update()` is
  reported as Failed.
- A newer commit clears a stale Empty / Failed / PermissionBlocked verdict.
- *Ignore* is persisted in `update_dot_dismissed_v1` in the state dir, keyed on the remote commit; a
  newer commit re-arms it.
- Idle text never claims "you are current" from silence: the portal only announces availability.
- **Changelog is lazy:** when shown, one short-timeout libcurl GET of
  `api.github.com/repos/properrr/deckback/releases`, keep versions newer than the compiled
  `kDeckbackVersion`, normalise the Keep-a-Changelog markdown to 10-foot text; on failure, a releases
  link. The release-notes contract (tag `v<X.Y.Z>`, body = the CHANGELOG section) is in `RELEASING.md`.

## Consent seed — Game Mode has no Access-portal backend

flatpak-portal asks the **Access portal** for consent before deploying a self-update, and **SteamOS
Game Mode has no working Access implementation**: SteamOS points portal loading at
`/usr/share/xdg-desktop-portal/gamescope-portals` (`XDG_DESKTOP_PORTAL_DIR`); `gamescope.portal` there
declares `org.freedesktop.impl.portal.Access` but lacks the `UseIn` key, so it fails to load
(`Error loading gamescope.portal: Key file does not have key "UseIn" in group "portal"`), and
`holo.portal` does not declare Access at all. Without the seed the deploy fails with **`No portal
support found`** (reproduced on-Deck, flatpak 1.16.6).

**Mechanism (traced in flatpak `portal/flatpak-portal.c`, 1.14.x and 1.16.x):**
`request_update_permissions_sync()` → `get_update_permission(app_id)` reads the xdg-permission-store,
table `"flatpak"`, id `"updates"`, and maps `perms[0]`: `"yes"` → YES, `"ask"` → ASK, anything else →
NO, no entry → UNSET. **The Access dialog is consulted only for UNSET or ASK**; YES proceeds with no
dialog; NO fails with "Application update not allowed". Seeding `yes` writes exactly what the dialog's
"allow" would.

**The seed is writable by us:** xdg-permission-store's `SetPermission(s table, b create, s id, s app,
as permissions)` does no caller validation, so the sandboxed launcher can record the consent given
`--talk-name=org.freedesktop.impl.portal.PermissionStore` (in the manifest). The user's consent is
*Update now* in notify mode (the opt-in itself in auto); the launcher writes only what the
unreachable dialog would record.

`seed_update_permission()` runs on the loop thread before the monitor is created: read the app id from
`/.flatpak-info` (`[Application]` `name=`), `Lookup` the current value, then — `yes`: nothing; **`no`:
respected, never overwritten** (warn + the `flatpak permission-set` hint); unset/`ask`:
`SetPermission` → `yes`. The pure decoders (`decode_update_permission`, `update_permission_name`,
`parse_flatpak_app_id`) are pinned by `updater_test.cpp` against flatpak's semantics (first element
only, case-sensitive, unknown → No).

Host-side equivalent, no rebuild: `flatpak permission-set flatpak updates io.github.properrr.deckback
yes` as user `deck`; inspect with `flatpak permissions flatpak`.

**Rejected: fixing the Access backend.** The portal dir is in the read-only rootfs, flatpak-portal
has its own legacy `.portal` resolver, and even a loaded gamescope Access impl would need a dialog UI
nobody has verified in Game Mode. These environment facts still matter for any feature that genuinely
needs an Access dialog in Game Mode.

## ★★ Permission adds break self-update

**Read this before touching `finish-args`.** flatpak-portal refuses any self-update that widens the
sandbox (`portal/flatpak-portal.c` `transaction_ready()`, confirmed in the 1.16.0 source the Deck runs):

```c
/* Actual app updates need to not increase premission requirements */
if (type == UPDATE && g_str_has_prefix (ref, "app/"))
    if (... || flatpak_context_adds_permissions (old_context, new_context))
      g_set_error (..., "Self update not supported, new version requires new permissions");
```

`flatpak_context_adds_permissions` (`common/flatpak-context.c`) compares shares, sockets, devices,
features, **bus policies**, filesystems and generic policy — a talk-name counts. User `flatpak
override`s are not consulted, so there is no client-side workaround, consent path, or retry.

- The comparison is against **what is deployed on that user's machine**, and flatpak jumps straight
  to the newest commit. One addition strands every user installed before it through *every* later
  release until they update by hand.
- The failure arrives as `Progress` status 3 with a usable D-Bus error name;
  `is_permission_change_failure()` keys on the **name** (`org.freedesktop.DBus.Error.NotSupported`),
  not the gettext-translated message. The Updates tab then shows the PermissionBlocked text and offers
  no button (every one would fail identically).
- **Guards:** `scripts/check-permissions.sh` (in `just preflight`, so every push) fails on any
  `finish-args` addition against every `v*` release tag; `publish-repo.sh` also gates the bundle's
  built `metadata` against the **live** repo head (exit 2). `ALLOW_PERMISSION_ADD=1` is the deliberate
  override, and a release using it must tell users to re-run the installer.
- **Recovery:** re-run the one-line installer in Desktop Mode (`scripts/web-install.sh`:
  `flatpak install --user -y --or-update deckback …`). It runs on the **host**, so the portal's rule
  does not apply and the permission change is auto-confirmed. It refuses, with instructions, when the
  app was installed from a different remote.
- **Never add `--talk-name=org.freedesktop.Flatpak`** to run host flatpak commands; that addition would
  itself strand every installed user. v0.0.7 shipped one addition (`--talk-name=org.freedesktop.systemd1`);
  0.0.8 removed it, and the keep-awake helper now reports liveness through a heartbeat file instead
  (`keep-awake.md`; verified in both directions on the OLED).

## ★ RENDER FIX — overlay styling under the page CSP

youtube.com/tv's CSP `style-src` has **no `'unsafe-inline'`**, so the browser silently drops both an
inline `style` attribute (`setAttribute('style', …)` or HTML-parsed `style=""`) **and** a `<style>`
element: a node styled that way is present in the DOM but unstyled (measured on-Deck: a fixed
badge rendered as a 1279×0 in-flow strip with computed `z-index:auto`). The exempt paths are
**CSSOM writes** (`el.style.setProperty`) and **`adoptedStyleSheets`**. The toast, the OSD and its
Settings button use only those, and `scripts_test.cpp` asserts they never regress to the blocked
forms. `config/scripts/overlay.js` (the onboarding controls card) still builds a `<style>` block and
is latently affected the same way.

Leanback can detach a `documentElement` child with an in-page body swap that does **not** fire the
navigator's `on_app_loaded` (SPA route changes keep the URL). Anything that holds input modal while
its node is shown must tie input capture to what is actually painted, or input stays trapped behind
an absent overlay; the OSD does this structurally (capture ⇔ paint).

## Design decisions

- **Reconnect, then inert.** The loop reconnects to the session bus with backoff and re-creates the
  monitor when flatpak-portal restarts (`NameOwnerChanged`); it goes inert for the session only after
  repeated failures, and relaunch starts fresh (`durable/dbus-reconnect.md`; verified in the sim,
  on-Deck drive pending).
- **Idle costs no battery.** The loop's `poll()` timeout comes only from sd-bus's own deadline; with
  nothing pending it blocks until a portal signal or the stop nudge — no periodic wakeups.
- **No reentrancy.** `Update()` is invoked from the loop body, never from a signal callback, so
  `sd_bus_call()` is never re-entered inside `sd_bus_process()`.
- **Toast guard (auto).** "Restart to apply" fires on `Progress` Done only for an update *we*
  initiated, so an externally-initiated Done cannot toast.
- **Deploy timeout in the UI**, not the updater: Requested times out after 10 min without a verdict.

## Release pipeline and the published repo (`scripts/publish-repo.sh`)

- **Signed.** The live `.flatpakrepo`/`.flatpakref` inline `GPGKey`; `summary.sig`/`summary.idx.sig`
  are served. The key is EdDSA `E16880BE44008CDC`, held only as an Actions secret, with no expiry
  (`RELEASING.md`). This matters because flatpak refreshes remote config (`xa.redirect-url`, keys) only
  on signed remotes, so moving hosts or rotating keys stays possible.
- **Linear history.** A publish mirrors the live repo (head + `DECKBACK_REPO_DEPTH`=2 parents), gates
  permissions against the live head, runs `build-commit-from --timestamp=NOW` on top of it, generates
  the from-parent delta with ostree directly, then `build-update-repo --generate-static-deltas --prune
  --prune-depth=2`. Each release's parent is the commit users actually have, there is a rollback
  target, and objects a CDN-cached summary (`max-age=600`) still points at survive a deploy.
- **Fails closed.** An unreachable live repo exits 3; `DECKBACK_REPO_FRESH=1` (workflow input
  `fresh_history`) starts a new history only on purpose. Re-publishing the live tree makes no commit.
- **Delta size depends on bsdiff for the engine.** ostree's default delta does not bsdiff files above
  its size cap, so the ~159 MB `content_shell` ships whole whenever it changes. Measured on the real
  v0.0.8 → v0.0.9 bundles:

  | Delta | Size |
  |---|---|
  | from-scratch | 77.1 MB |
  | from-parent, flatpak defaults | 66.7 MB (63.8 MB of it the engine binary as a fallback object) |
  | from-parent, `--max-bsdiff-size=512 --min-fallback-size=512` (what we publish) | **4.2 MB** |

  Generating it took 40 s at 2.5 GB peak RSS (fine on the 16 GB Actions runner). A release that
  leaves the engine untouched gets a small delta either way. A repo holding two releases is 145 MB
  against Pages' 1 GB.
- **Verified off-device:** a real `flatpak --user` client on 0.0.8 updated to 0.0.9 from a locally
  signed repo via the bsdiff delta (4.2 MB, no whole-object fetches, exact published commit); a
  read-only run against production mirrored with `--depth=2` even though the live head's parent 404s;
  `tests/harness/test_publish_repo.sh` (27 checks, green on flatpak 1.18.1/ostree 2025.6 and Ubuntu
  24.04's 1.14.6/2024.5) covers delta use (from the server's request log), a client two releases
  behind, recall as an ordinary update, `update --commit` back one release, `mask`, a permission add
  (exit 2) and an unreachable repo (exit 3).
- **Recall:** `RELEASING.md` §"Recall a bad release" — re-commit the last good tree forward with a
  fresh timestamp (the portal refuses downgrades, and re-publishing an old bundle as-is would offer
  every user an update that can never install) by re-dispatching *release-pages* with the last good
  tag, and turn the bad GitHub release back into a **draft** (not pre-release), or the Updates tab's
  changelog keeps offering it. `flatpak mask` is the user-side "hold this version".
- **The runtime and GL.default (Mesa, i.e. the VA-API driver) refresh only alongside a real Deckback
  update**, and only if they live in the same (user) installation (flatpak source; not observed
  on-Deck). Nothing on SteamOS updates Flatpaks in Game Mode; a system-wide runtime is never refreshed
  by us.
- `--version` and startup log the real version, compiled in from the repo `VERSION` file.

## ★★★★ Verification status — what is pending

- **T1 (Deck, staging repo, `DECKBACK_UPDATE_REF_URL`):**
  - Launch with a newer staging commit: the badge appears within seconds of start.
  - Check reads "latest" and installs nothing; the journal has no "deploy requested".
  - *Update now* shows progress, then *Close now* exits cleanly; relaunch from the Library runs the new
    commit.
  - Check → Empty, then publish a newer commit: it is offered.
  - With a permission-adding staging commit: the tab shows the blocked text, and re-running the
    installer in Desktop Mode installs it. (The Failed / PermissionBlocked text has never been read on
    a Deck, only by unit tests.)
- **T2 (production): passed with v0.0.10 (2026-10-21).** The signed *release-pages* run logged
  `Live head: 7869c172…` and `Published 3147f33… (parent 7869c172…)` past the permission gate. The
  OLED Deck on 0.0.9 was offered the update by the portal (notify mode), deployed it from
  Settings ▸ Updates in 3 s and relaunched into 0.0.10. With the engine unchanged the update is a
  148 KiB delta; a fresh install pulls 75.6 MB.
- The reconnect on-Deck drive (`dbus-reconnect.md`).

## ★★★ Open items (REVIEW 2026-10-20 plan, U1–U11 in TASKS Phase 8)

U1–U6 are done off-device (above). Still open:

- **U7** — stale runtime and Mesa in Game Mode (a host timer; owner decision).
- **U8** — install paths diverge: Releases bundles carry no `--repo-url` and never update. A bundle
  built with `--repo-url` installs as a `deckback-origin` remote, which `channel_of_origin` treats as
  the DEV channel and `web-install.sh` refuses; the dev and release origins must be told apart by URL
  first.
- **U9** — no kill-switch or incident banner. **U10** — no beta channel.
- **U11** — Pages' soft 100 GB/month is ≈1,300 updates/month at a full-size 77 MB update (far more at
  bsdiff-delta sizes).
- **Key custody** — one never-expiring key, CI-only, no rotation plan (owner decision).

**None of the plan may add a sandbox permission** (§"Permission adds break self-update").

## Testing on a Deck — mechanics worth knowing

- **Staging repo:** `just publish-repo` + `python3 -m http.server`, added as a `--user
  --no-gpg-verify` remote (`RELEASING.md` §"Test the repo locally").
- **Fast portal poll:** `just portal-poll set 60` installs a user drop-in
  (`~/.config/systemd/user/flatpak-portal.service.d/zz-deckback-fastpoll.conf`,
  `ExecStart=/usr/lib/flatpak-portal --poll-timeout=60`; built-in default ~30 min); `just portal-poll
  restore` removes it; `status` reports the interval. Set it on the service, not a hand-started
  instance — D-Bus re-activation replaces a manually started portal. Run `set` **before** launching
  Deckback.
- **Diagnostics:** `deckback-launcher --selftest-update` (monitor creation + stored permission),
  `--selftest-deploy` (one Update cycle, raw permission) and `--selftest-deploy-seed` (seed first) — a
  scriptable A/B of the portal. `DECKBACK_FAKE_UPDATE=<commit>` publishes availability at launch.
- **Launch through the Steam shortcut** (`steam://rungameid/<id>`): a bare `flatpak run`/SSH/systemd
  launch renders off-screen behind the Steam UI in Game Mode.
- **CDP `Page.captureScreenshot` under gamescope is unreliable for overlays** — it has returned the
  page without injected nodes the DOM confirmed present. Trust DOM assertions
  (`getBoundingClientRect`, `getComputedStyle`) and a human's eyes.
- `~/.local/bin` is not on a non-interactive SSH `PATH`; call host helpers by full path.
