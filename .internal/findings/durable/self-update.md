# Self-update via the Flatpak portal (launcher/src/updater.cpp)

## ★ UI MOVED INTO THE OSD (2026-07-15) — see osd-menu-plan.md

The standalone notify overlays described below (the amber **pill** and the modal **card**,
`update_badge*.js`/`update_card*.js`, and `input.cpp::handle_update_card`) are **removed**. The
detect/deploy engine (`updater.cpp`, `UpdateState`) and `updateprompt.cpp`'s
detection/changelog/consent logic are **unchanged and still the source of truth** — but the UI is now
the in-app **OSD Settings menu** (`.internal/osd-menu-plan.md`): `updateprompt.cpp::tick` feeds
`OsdMenuController::set_update_state`, which draws the amber badge on the Settings button and the
**Updates** tab; `confirm_update()`/`ignore_version()` are OSD action callbacks (A on *Update now* /
Y / focused *Ignore*). This also fixes BUG 2b structurally (capture ⇔ paint, `osd-menu-plan.md` §2).
Everything below about the portal deploy, the permission-store seed, and the on-Deck T1 results still
holds verbatim; only the notify *surface* changed. The on-Deck notify round-trip is still pending —
now via `tests/deck/test_osd.py` + the update round-trip, not the deleted pill/card.

## ★ NOTIFY MODE is the default (2026-07-14) — detect, don't enforce

The config key is now **`self_update_mode`: `notify` | `auto` | `off`, default `notify`** (the legacy
boolean `self_update` still parses: `true`→`auto`, `false`→`off`). The portal detect+deploy machinery
below is unchanged and still the engine; what changed is the *trigger* and a launcher-owned UI:

- **detect → notify → deploy** is split (`updater.cpp`). In `notify`, `on_update_available` no longer
  sets `want_update_`; it publishes availability into a shared `UpdateState` (thread-safe: an atomic
  `available`, a mutex-guarded remote commit) and waits. Deploy happens only when
  the user consents, via `Updater::request_update()` (sets an atomic, nudges the loop; the loop — never
  the caller — issues `Update()`, preserving the no-reentrant-`sd_bus_call` rule). `auto` keeps the old
  behaviour (deploy on detection). The consent **seed still runs in both** notify and auto, so a
  user-confirmed notify deploy takes the same no-dialog path as auto (★ SOLUTION below).
- **UI is launcher-drawn, not YouTube's DOM** (`launcher/src/updateprompt.cpp` + `config/scripts/
  update_badge*.js`, `update_card*.js`), so a Leanback frontend change can't break it — same principle
  as the controls card. The corner indicator is a **pill** — an amber dot + "Update available" + a
  **☰ keycap** telling the user the card opens from the Menu button — appended to `documentElement`.
  The input thread's `tick()` reconciles it each poll: shown when an un-dismissed update exists AND
  no video is up, and **hidden while a video plays** so it never sits over playback; it returns on
  browse. The watch signal is `LayerState::video_up` (the raw `player_open` bit), deliberately NOT
  `layer() == Layer::Player`: the OSK outranks the player in `resolve_layer()` while a video can
  still be playing underneath, so the layer alone would draw the pill over playback during text
  entry (2026-07-14 review). `tick()` is the **sole owner of the pill DOM** — dismiss handlers only
  clear `dot_desired_` and let the same-iteration reconcile hide it. `on_page_reloaded()` (navigator
  thread) flags a full-reload wipe so `tick()` redraws it (playback-aware) and only the input
  thread's `client_` ever touches it. Two accepted timing windows (both bounded by polls, judged
  imperceptible-to-minor): the pill can sit over a just-started video for up to one player-poll
  interval (`devtools_poll_ms`, 1000 ms) because `#/watch` is an SPA route change the poll must
  observe, and after a full reload the redraw waits for the input loop's next poll wake (≤1–2 s).
  Once per *new* commit an **"Update available" card** auto-shows (modal, owned by the input thread
  like the onboarding card) — **also gated off playback**: the card is modal (it swallows direction
  keys), so `want_card_` stays armed while a video is up and the card pops only once the user leaves
  the watch view (2026-07-14 review; before that only the pill was gated). Fixed buttons **A** =
  Update now (`request_update()` + "restart to apply" toast), **B** = Not now (keep the dot), **Y**
  = Ignore this version (hide the dot until a newer commit — persisted solely in the
  `update_dot_dismissed_v1` marker; the old in-memory `UpdateState::dot_suppressed_` lost its last
  reader in the pill rework and was deleted). The **Menu (☰)** opens the card any time an update
  exists — reachable even after the dot is dismissed, and deliberately allowed over playback because
  it is user-initiated.
- **Painless + ignorable + reachable** is enforced by two versioned state files in the state dir,
  keyed on the portal's **remote commit** (precise dedup; version labels are cosmetic to the portal):
  `update_card_shown_v1` (suppresses the one-time auto card) and `update_dot_dismissed_v1` (hides the
  dot). `decide_notification()` maps (commit, card_shown, dot_dismissed) → {show_card, show_dot}; a
  newer commit re-arms both. A manual Desktop-Mode `flatpak update` binds the new commit on next
  launch → the portal reports nothing → no dot/card, and the card's up-to-date text reads "You're on
  the latest version".
- **Changelog is lazy** (`updateprompt.cpp::changelog`): only when the card is displayed does the
  launcher do one short-timeout libcurl GET of `api.github.com/repos/properrr/deckback/releases`,
  parse it (`parse_github_releases`), keep versions newer than compiled `kDeckbackVersion`
  (`compare_versions`), and normalise the Keep-a-Changelog markdown to 10-foot text
  (`notes_to_plain`). No launch-time network; on any failure the card falls back to a releases link.
  The release-notes contract the updater depends on (tag `v<X.Y.Z>`, body = the CHANGELOG section) is
  documented in `RELEASING.md`.
- **Tests:** pure helpers in `launcher/tests/updateprompt_test.cpp` (version compare, releases parse,
  changelog summary + markdown normalise, the show-card/show-dot decision incl. a mandatory suppress
  case, marker round-trip + write-failure tolerance) and `UpdateState`/`request_update` lifecycle in
  `updater_test.cpp`. On-Deck verification of the card/dot/deploy round-trip is still pending (the
  portal deploy itself is proven — ★ T1-REPLAY below). **Update 2026-07-14: the notify UI round-trip
  WAS run on-Deck — the logic fires, but BOTH overlays fail to render on-device; see ★ NOTIFY UI
  ON-DECK below. ROOT-CAUSED and FIXED 2026-07-14 (★ RENDER FIX below): both bugs were the CSP
  `style-src` (no `'unsafe-inline'`) silently dropping our inline-style / `<style>` injection — the
  same block `no_pointer.js` already works around. The overlays now style via CSSOM +
  `adoptedStyleSheets`, and the card self-heals so it can't strand input. Code + unit tests land here;
  the on-Deck re-run of the round-trip is the remaining gate (implemented, not yet verified).**

## ★ NOTIFY UI on-Deck 2026-07-14 (OLED, Game Mode) — logic fires, overlays DON'T render (2 bugs)

Ran the notify round-trip against the OLED Deck. Setup: a local archive-z2 staging repo served over
HTTP from the workstation (`192.168.128.3:8099`, `python -m http.server` on `flatpak/repo`); installed
a **notify-mode base** (0.0.4, `self_update_mode: notify`) on the Deck from a `--user --no-gpg-verify
deckback-staging` remote, then published a newer commit (0.0.5) into the same repo. `flatpak update`
saw it (`u`), confirming detection works at the ostree layer. Because the portal's default check is
**30 min** (see ★ T1 RESULT) and a `flatpak-portal.service --poll-timeout` drop-in was out of scope,
the notify UI was triggered with the built-in **`DECKBACK_FAKE_UPDATE`** hook (`main.cpp`) set via
`flatpak override --user --env=...`, launched through the **Steam shortcut** so gamescope actually
focuses the window (a bare `flatpak run` / SSH / systemd launch renders off-screen behind the Steam UI
— it is NOT visible in Game Mode; only `steam://rungameid/<id>` is).

**What WORKS (journal-confirmed, `journalctl --user`):** `startup: self-update mode = notify` · the
fake-update seed (`DECKBACK_FAKE_UPDATE set — simulating an available update`) · `updater: watching
for updates … via the Flatpak portal` · the lazy GitHub fetch correctly falling back (`update: could
not fetch release notes from GitHub — showing a link instead`, since no `v0.0.5` release exists) ·
`update: 'update available' card shown`. CDP confirmed both nodes were injected into the live Leanback
DOM.

**BUG 1 — the dot mis-renders.** `#__deckback_update_dot` has computed `display:block` but
`getBoundingClientRect()` = **1279×0 at (0,799)** — a full-width, zero-height strip at the bottom-left,
not a 14×14 amber circle top-right. `childCount:0`, `z-index:auto`. The inline styling from
`config/scripts/update_badge.js` (position/top/right/width/height/border-radius/background/z-index) is
**not applied**. The controls-card/toast overlays DO render on-Deck, so `update_badge.js` differs from
`toast.js`/`overlay.js` in how it sets style (likely the style is applied in a form Leanback's CSP /
Trusted Types strips, or set on the wrong node). Fix by mirroring the proven toast/overlay injection.

**BUG 2 — the card is invisible and traps input.** A composited screenshot showed only the YouTube
home; the card never visibly painted although its node existed. Moments later the card node was **gone**
(`getElementById → null`) — removed by a Leanback DOM swap — while the launcher still held
`card_visible_ = true`, so the input thread kept **swallowing direction presses** (user: "I don't see
anything and keys doesn't work"). Two defects: (a) the card doesn't paint on-Deck (same class of issue
as BUG 1 — compare to onboarding `overlay.js`, which does render); (b) `card_visible_` desyncs from the
DOM — a `documentElement`-appended card does NOT reliably survive Leanback body swaps here, so it must
be re-injected on `on_app_loaded` (like the dot's redraw) AND/OR removal must reset `card_visible_` so
input is never trapped.

**Caveat for whoever picks this up:** CDP `Page.captureScreenshot` under gamescope is **unreliable for
overlay verification** — it returned the YT page WITHOUT the injected nodes even when the DOM confirmed
them present. Trust DOM assertions (`getBoundingClientRect`, `getComputedStyle`) and a human's eyes,
not the screenshot, when checking on-Deck overlays.

**Code review (2026-07-14, no device) — the overlays are structurally identical to the working ones,
so there is NO static defect to fix; the cause is runtime/environmental.** Side by side:
`update_badge.js` styles via `setAttribute('style', …)` exactly like `toast.js` (which renders);
`update_card.js` uses a `<style>` block + the memoised `window.__dbTTP` Trusted Types policy exactly
like `overlay.js` (the controls card, which renders). The injection is the same too — `draw_dot()` /
`show_card()` call `ScriptLibrary::instance().invoke(client_, …)`, the same path `show_toast` and the
onboarding card use. The scripts DID run on-Deck (node present, `card shown` logged); only the
**styling failed to take effect** — the dot's computed `z-index` was `auto`, not `2147483647`, i.e. the
`style` attribute was dropped. Byte-equivalent-to-working code failing means it is not a source bug.

**Discriminating experiment for the next host (settles it in ~2 min on the live page):** draw a toast
AND the dot, then compare —
`getComputedStyle(document.getElementById('__deckback_toast')).zIndex` vs
`…('__deckback_update_dot').zIndex`, and read
`document.querySelector('meta[http-equiv="Content-Security-Policy"]')?.content` for `style-src`.
- If BOTH are `auto`: inline styles are now globally CSP-blocked (a Leanback change; the "toast works"
  claim is stale). Fix = a CSP-exempt style path — inject via CDP `Page.addStyleSheet`, or a
  constructable `CSSStyleSheet` + `document.adoptedStyleSheets` — NOT inline `style`/`<style>`; and the
  SAME fix then applies to `toast.js`/`overlay.js`.
- If only the dot is `auto`: a timing/context race (our overlays are drawn from `tick()` as soon as
  `UpdateState::available()` flips — with `DECKBACK_FAKE_UPDATE` that is at startup, mid Leanback boot,
  whereas toast/controls-card only ever show post-load on a user action). Fix = gate the initial
  draw on `on_app_loaded` and re-inject the CARD on reload (today only the dot has
  `redraw_dot_on_reload`).

Independently of rendering, BUG 2 has a real code defect: `card_visible_` is never reconciled with the
card's DOM presence, so a Leanback swap that removes the node leaves the input thread trapping
direction keys. Whatever fixes rendering must also make card removal reset `card_visible_` (or
re-inject), so input can never be trapped behind an absent card.

**Next machine:** run the discriminating experiment above, pick the matching fix, then re-run the
round-trip with *real* portal detection (a `flatpak-portal.service` drop-in
`ExecStart=/usr/lib/flatpak-portal --poll-timeout=15`, per ★ T1-REPLAY) and drive A=confirm → real
deploy from staging. The deploy path itself is already proven (★ T1-REPLAY); only the notify UI is the
gap.

## ★ RENDER FIX 2026-07-14 (no device) — it was the CSP `style-src`, resolved from the touch-lock finding

The discriminating experiment above did not need a device after all: `durable/touch-lock.md` already
records the answer, verified on-Deck 2026-07-10. On the current pin youtube.com/tv's CSP `style-src`
has **no `'unsafe-inline'`**, so the browser silently drops **both** an inline `style` attribute
(`setAttribute('style', …)`, and HTML-parsed `style=""`) **and** a `<style>` element. That is exactly
why `no_pointer.js` hides the cursor through a **constructable `CSSStyleSheet` + `adoptedStyleSheets`**
instead of a `<style>` tag. Map it onto the two bugs and both fall out with no ambiguity:

- **BUG 1 (dot):** `update_badge.js` styled via `setAttribute('style', …)` → CSP-dropped → computed
  `z-index:auto`, an unstyled in-flow block (the 1279×0 strip). ✓
- **BUG 2a (card invisible):** `update_card.js` styled via a `<style>` block → CSP-dropped → the
  container had no `position:fixed`/backdrop, so it never painted. ✓
- The "toast/controls-card render on-Deck" claim that made this look like a dot-only defect is from
  **m114** (CLAUDE.md attributes it to `m114.md`), i.e. **stale on M138** — `toast.js` used the same
  blocked `setAttribute('style', …)`, so the feature's own "Updating…" confirm toast was latently
  broken too. Two exempt paths exist and only these: **CSSOM writes** (`el.style.setProperty`, not
  subject to CSP) and **`adoptedStyleSheets`**. Both were already proven on-Deck by `no_pointer.js`.

**The fix (shipped in this branch, unit-tested; on-Deck re-run still pending):**
- `config/scripts/update_badge.js` — every property set via `el.style.setProperty(...)` (CSSOM), no
  `style` attribute. `config/scripts/toast.js` — same conversion (its fade already used `.style`).
- `config/scripts/update_card.js` — descendant rules moved into a constructable `CSSStyleSheet` added
  to `document.adoptedStyleSheets` (memoised on `window.__dbCardSheet`), plus a CSSOM fallback on the
  container so the dark modal backdrop paints even where constructable sheets are unavailable. The
  `<style>` block is gone; the Trusted-Types innerHTML for the text content stays.
- **BUG 2b (input trap), fixed independently of rendering:** a Leanback *in-page* body swap can detach
  a `documentElement` child, and that swap does **not** fire the navigator's `on_app_loaded` (it only
  fires when `location.href` re-enters the app needle — SPA route changes keep the same URL). So a
  shared **keep-alive `MutationObserver`** (`window.__dbKeepAlive`, defined identically in
  `update_badge.js`/`update_card.js`) re-appends our nodes when detached; the `*_hide` scripts call
  `window.__dbDropAlive` **first** so a deliberate hide isn't fought. For a *full* navigation (new page
  global, observer gone) the launcher re-injects: `UpdatePromptController::redraw_card_on_reload()`
  (navigator thread, own transient `DevToolsClient`, no state/marker side effects) mirrors the dot's
  `redraw_dot_on_reload()`, wired next to it in `main.cpp`'s `on_app_loaded`. `card_visible_` is now
  `std::atomic<bool>` for that cross-thread read. Net effect: the card can no longer vanish while
  `input.cpp` still treats it as modal and swallows direction keys (the "keys don't work" report).
- **Tests:** `scripts_test.cpp` now asserts the launcher-drawn overlays never regress to the blocked
  paths (`update_badge`/`toast` use `.setProperty` and never `setAttribute('style'`; `update_card` has
  no `<style>` and uses `adoptedStyleSheets`) and that the card/dot carry the keep-alive observer while
  the hide scripts drop from it. `overlay_test.cpp`'s toast-shape assertions updated to the CSSOM form.

**Still TODO (the real gate):** re-run the on-Deck notify round-trip (Steam-shortcut launch,
`DECKBACK_FAKE_UPDATE`, DOM assertions per the caveat — not the unreliable gamescope screenshot) and
confirm the amber dot, the modal card, and the confirm toast all paint, and that a Leanback DOM swap
no longer traps input. **Follow-up (separate, same cause):** `config/scripts/overlay.js` (the
onboarding controls card) still uses a `<style>` block and is latently broken on M138 the same way —
it belongs to onboarding, not this branch, but should get the identical `adoptedStyleSheets` treatment.

Everything from here down documents the **portal deploy mechanism** (shared by notify + auto) and its
on-Deck verification. It predates the notify split; where it says "`self_update=true` auto-deploys",
read that as **`auto` mode** — the mechanism is identical, only the trigger differs.

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
