# Deckback OSD Settings menu — design doc

Status: **IMPLEMENTED** — the framework (tabs, Settings ▸ Keys read-only, Updates, About) shipped,
and as of 2026-07-22 the **first WRITABLE sub-tab landed: Settings ▸ Captions**, together with the
`user.json` sparse overlay (configurator C2) it writes through — `launcher/src/config_store.cpp`
(the codebase's first config writer, atomic tmp+fsync+rename, allowlisted keys) and
`caption_settings.cpp` (the shared caption model). The Captions sub-tab edits an ordered preferred-
language list (add/remove + a full-language picker), the author/auto source policy, remember-last,
and the toast; each edit returns the non-closing `apply:cc.<key>=<val>` verdict and persists live.
Widget model (combo + langlist + picker) verified on-Deck over CDP. Remaining writable settings
(button remap, hardware-decode toggles, reset-to-defaults) are still roadmap. Original design below.

Status (original): **DESIGN — nothing implemented.** Branch `feature/osd-settings-menu`. Written 2026-07-15
against launcher/config state at `main` commit `79eae9b` (Release v0.0.4). **Amended 2026-07-15
after code-grounded reviews:** §12 is restructured around the tiers that actually exist (the focus
model gets an executable L1 suite — there is no JS engine at L0), performance contract added
(O12, §4.1/§4.2), shared overlay prelude added (O11, §4.4), and the `overlay.js` CSP inconsistency
is flagged for resolution (§11, §15). The current-app review also makes the C++↔JS gateway,
reload lifecycle, fixed Menu entry point, update-mode states, and A/B/Y contract explicit
(O13–O16, §4, §5, §9–§12). Supersedes the standalone
self-update notify UI (`updateprompt.cpp` + `update_badge*.js`/`update_card*.js`) by folding it into a
tab of this menu. The main plan `.internal/steamdeck-cobalt-youtube-plan.md` wins on conflict for the
YT app itself; this doc owns the in-app OSD.

**This design supersedes `.internal/configurator-plan.md`** (2026-07-15, user decision). That doc
planned a *separate companion app* ("Deckback Settings") to edit config on disk; the OSD replaces it
with a single **in-app** settings surface. The configurator's load-bearing decisions — the sparse
`user.json` overlay, the "user config must not shadow the shipped R1 hotfix keys" rule, button-remap
conflict protection, and reset-to-defaults — are migrated here as the OSD's writable-settings roadmap
(§11). `configurator-plan.md` is marked superseded and kept for history only.

## 1. What and why

An **on-screen overlay ("OSD") menu** drawn over the running YouTube TV client, opened from a
persistent top-right **Settings** button (the amber corner element the self-update pill used to be),
and driven entirely by the controller. It is the app's "internal engineering menu" — the one place
we surface app state and, over time, app settings, without leaving Leanback.

v1 has **two top-level tabs**:

1. **Settings** — a tab with N sub-tabs. v1 ships exactly one sub-tab, **Keys**: the current
   controller hot-keys in use, rendered from the *live* keymap (never hardcoded). More sub-tabs are
   added later; the shell is built to absorb them without rework.
2. **Updates** — the active update policy and state: notify-mode availability/changelog/consent,
   automatic-update status, or an honest "updates are off" instruction. It never claims a version is
   latest solely because no availability signal has arrived.

Why now — three problems converge:

- **The self-update UI has no home.** Today the amber pill and the update card are one-shot, modal,
  and reachable only via the Menu button *when an update exists*. Pressing Menu with no update falls
  through to the controls card (`open_from_menu()` returns false → `onboarding_->show()`), which is
  the surface of a reported input-trap (§2, bug b). A permanent Settings surface removes the
  dead-end: the button always opens a well-defined menu.
- **We keep bolting overlays on individually.** Toast, controls card, update pill, update card, error
  page — five injected-DOM systems, each re-deriving CSP-safe styling, keep-alive, and modality. The
  OSD is the consolidation point: future surfaces become *tabs*, not new top-level overlays.
- **There is nowhere to put settings in-app.** The companion configurator (`configurator-plan.md`) is
  a separate app; users in Game Mode want a couch-reachable in-app menu. The OSD is that surface. v1
  is read-only (Keys) + Updates actions; the widget/focus framework is built now so writable settings
  drop in later (§11, C-write).

Branding: the button is labelled **"Settings"** and keeps the current menu/gear glyph. Never
"YouTube"/"YT" anywhere in it.

## 2. The prior bugs this design must not reintroduce

These drive the test plan (§12). All are input/overlay failures.

**(c) Capture ⇔ paint desync across a page reload / sleep-resume (found + fixed on-Deck
2026-07-15).** Open the menu, then reload the page (a Game-Mode suspend/resume, or any
`location.reload()`): the OSD's JS context is wiped and keep-alive cannot bring the node back, yet
the launcher kept `open_ == true` and went on capturing input — so every key drove Leanback *behind*
an absent menu. The reverse (keep-alive preserves the node across an SPA body swap while `open_`
went false) yields the opposite: a *visible* menu the launcher no longer owns, keys passing through
it. Root cause: `on_page_reloaded()` only re-synced on the navigator's not-app→app transition, which
a **same-URL reload never fires**. Fix: the **input-thread `tick()` is the single owner** and
reconciles every iteration — a throttled `op:"state"` poll while `open_` distinguishes `gone` (JS
context wiped → release capture) from `detached` (keep-alive will re-append → keep capture) from a
live state string; `on_page_reloaded()` now only sets a flag. Covered by `osdmenu_test.cpp`
(gone/painted/detached) and `test_osd.py::test_capture_released_when_a_reload_wipes_the_open_menu`.
**Lesson: capture and paint are one fact, reconciled continuously — not synced once at a DOM event.**
The `gone`/`detached` distinction is not only the reconcile poll's — `op:"cmd"` must honor it too: a
keypress landing in a detach window used to return `gone` and release capture, stranding the
re-appearing painted node with no owner (passthrough). A cmd on a detached-but-kept-alive node now
returns `consumed` (keep capture); only a truly orphaned node (dropped from keep-alive, i.e. closed)
reports `gone`. Covered by `test_osd.py::test_cmd_during_a_detach_window_keeps_capture`.

**(a) Off-watch action on the home screen (`durable/input-ux.md` §18 ★ BUGFIX 2026-07-14; commit
`0ab5fb7`).** The seek scripts located the player by `querySelector('.html5-video-player')` alone.
Leanback keeps that element in the DOM (backgrounded, paused) after the user backs out to the home
screen, so pressing L2/R2 there *resumed the previous video audibly behind the home screen*. Fixed by
gating on `location.hash.indexOf('/watch') >= 0`. **Lesson for the OSD: every action is
context-gated.** An action offered in the menu must check the same live signals the input layer uses
(`video_up`, `location.hash`) at the moment it fires — never act on stale state, never act off the
context that makes the action meaningful.

**(b) Menu-with-no-update trapped input (undocumented variant of `self-update.md` BUG 2b).** The
update card is modal in `input.cpp::handle_update_card` — while `card_visible()` it **swallows every
event**, mapping only fixed A/B/Y. If `card_visible_` desyncs from the DOM (a Leanback body swap
removes the node while the flag stays true), the input thread keeps swallowing direction keys and the
user sees nothing and "keys don't work". The user's report — "press menu when no updates was blocking
inputs, expected A/B/Y" — is this class: the menu path armed a modal state whose UI wasn't actually
on screen. **Lesson for the OSD: modal input capture and DOM presence are one fact, not two.** The
capture predicate must be reconciled against the overlay actually being present and painted. A
navigation can race one already-arrived event, but absence must immediately drop capture once observed
and must never become a persistent invisible modal state.

## 3. Locked decisions

| # | Decision | Rationale |
|---|----------|-----------|
| O1 | The OSD is **CDP-injected DOM**, authored as `config/scripts/osd_*.js` through `ScriptLibrary`, rendered by a new **`OsdMenuController`** (C++), same stack as `onboarding.cpp`/`updateprompt.cpp`. No engine patch, no new dependency. | The overlay stack is proven on-Deck (controls card, update card render under M138 after the CSP fix). Anything else means a second UI toolkit and input path. |
| O2 | **Focus, tab/sub-tab selection, and scroll live in the injected JS** (`osd_menu.js`); C++ captures the pad (modal) and forwards **discrete commands** (`up/down/left/right/select/back/tab_next/tab_prev`). C++ owns open/close, playback gating, and action dispatch only. | Chosen over C++-driven re-render: fewer CDP round-trips per keypress, richer in-page focus traversal (roving tabindex, scroll-into-view), and the focus model is unit-testable as a pure JS component. **Caveat from review: no JS engine exists in the repo's test stack, so the harness that makes "unit-testable" true is a hard deliverable — the L1 suite in §12.** (User-selected, 2026-07-15.) |
| O3 | The menu is **derived from live in-memory config at open time** — single source of truth, like the controls card. No live file-watcher in v1. | User-selected (2026-07-15). A hot-swapped `app.json` reflects after the next launch; the menu is never hardcoded/stale, which is the real requirement. A watcher is a later, separable feature. |
| O4 | v1 is **framework + read-only Keys sub-tab + Updates actions**. Widget types (checkbox, combobox, button) are built but only Button is wired (Updates). **No config-write/persistence layer in v1.** | User-selected (2026-07-15). Writable settings land when the first real setting is defined and the shared `user.json` overlay (configurator C2) exists. Building the widgets now means the first setting is a data change, not a framework change. |
| O5 | The **Settings button is always present except during video playback**, and **always opens the OSD** (never a dead-end). It replaces the self-update amber pill. The physical **Menu/Start** control opens it off-watch; this entry point is fixed, not a remappable `show_controls` action. | The user's "yellow button active all the time except playback." Removes the `open_from_menu()==false` fall-through (bug b) and cannot disappear when first-run help is disabled or a future keymap is malformed. Playback gating reuses `LayerState::video_up()`, the exact signal the pill already used. |
| O6 | The **update pill/card/dot are removed as standalone overlays**; their logic (detection, changelog, decision, deploy consent) moves behind the **Updates tab** and a **badge on the Settings button**. `UpdatePromptController` becomes non-UI `UpdateCore`, not a duplicate controller. | One surface, not five. The "reach the update any time" guarantee is *strengthened* (the button is always there), not lost. |
| O7 | The OSD is **modal input capture** in `input.cpp`, slotted at the **top** of `handle_event` (before `handle_update_card`/`handle_onboarding`). Its capture predicate is `lifecycle == Visible`, not a bare "open" boolean. Every command response is a presence proof; an absent/error response atomically drops capture, and an explicit CDP probe runs only while `Visible` and input-idle — never as a steady poll. | The `Closed → Opening → Visible → Reloading` state machine fixes the stranded-modal class without claiming an asynchronous DOM navigation can be observed before it happens, and without turning the guard into a battery-hostile poll (O12). |
| O8 | Primary scroll is **D-pad / left stick** (focus move + scroll-into-view) and **right stick** (analog fast-scroll of the focused scroll region). **Right-trackpad circular scroll is a follow-up** pending an on-Deck evdev spike (§8). | Ships a working scroll on day one with input we already read; the trackpad path needs a Steam Input mapping + evdev spike we have not run. |
| O9 | The **fixed Deck-button semantics inside the OSD are keymap-independent** (resolved via `control_code()`): **A** = activate focused control, **B** = back/close (and therefore "Not now" for an available update), **Y** = ignore the available update, **D-pad** = move, **L1/R1** (or Menu) = switch tab. Y is consumed with no effect when Updates has no available update. The user's keymap does not rebind opening or navigation. | A settings menu whose own navigation could be rebound (and broken) by the config it edits is unusable. This preserves the old card's expected A/B/Y update semantics without making an invisible "Not now" button compete with B. |
| O10 | The OSD is **the single settings surface** and **supersedes the separate companion configurator** (`configurator-plan.md`). Everything the configurator planned — button remap, hardware-decode / touchscreen toggles, reset-to-defaults — becomes OSD Settings sub-tabs over time, writing the **same sparse `user.json` overlay** (configurator C2). v1 stays read-only (O4); the write path lands with the first real setting. | User decision 2026-07-15. One couch-reachable, in-app surface beats a second app the user has to find in the library and launch. In-app also unlocks *live* application of some settings (the keymap especially — input.cpp holds the resolved `Keymaps` in-process), which a separate disk-editing app cannot do. |
| O11 | All overlay scripts share **one prelude**: `config/scripts/db_common.js`, installed once per document as a **sticky document-start script** (`ScriptLibrary::install_sticky`, the `no_pointer.js` mechanism). It provides `__dbKeepAlive`/`__dbDropAlive`, the memoised Trusted Types policy `__dbTTP`, the HTML-escape helper, and a memoised constructable-sheet helper. `osd_menu.js`/`osd_button.js` use it instead of carrying private copies; `overlay.js` migrates onto it (§11). | The keep-alive + TTP infra is today **copy-pasted per script** (`update_card.js:18-42`, `update_badge.js:9-32`); §11 deletes those originals, which would have left the OSD's copies canonical by accident, and the OSD scripts would be copies four and five. One prelude kills the drift risk and shrinks the bytes evaluated per injection. The `scripts_test.cpp` keep-alive/TTP guards move to the prelude. |
| O12 | **Performance contract (battery):** with the menu closed — the steady state — the OSD costs **zero CDP calls, zero timers, zero DOM work**; `tick()` is atomic loads plus edge-detected button/badge draws. While open, the cost is one eval per input command, the scroll tick is capped (§8), and the JS side runs no timers/rAF loops and no compositor animations on focus moves (§4.2). | The OSD piggybacks on the input thread exactly like `UpdatePromptController::tick()`, which is cheap *only because* it is edge-detected (`updateprompt.cpp:397-401`). The measured 5.6 W P4 budget assumes overlays are free when idle; this contract keeps them free. |
| O13 | The C++↔JS boundary is one **ScriptLibrary command gateway**: `osd_menu.js` receives `{op:"open"|"exec"|"close"|"state", ...}` and always returns one JSON **string** reply: `{present, generation, verdict, focus}`. C++ never invokes `window.__dbOSD` directly. `ScriptParams` gains a typed `set_json()` path for the model, and `OsdReply` is the one small, fixed-schema parser. | `DevToolsClient::eval_string` only accepts primitive strings today, while `ScriptLibrary` only evaluates `(script)(params)`. A single string envelope works through both APIs, keeps C++ out of hand-built JS, and makes malformed, stale, or absent replies fail closed. |
| O14 | `db_common.js` is installed by **Navigator before its first navigation**, unconditionally whenever the CDP navigation stack is enabled; it is sticky across target recreation. | A post-load installation is too late for the first-run card and contradicts the prelude's defensive no-op rule. Navigator is the existing owner of document-start scripts. |
| O15 | The Updates model explicitly represents the configured policy: **off** = no check / direct the user to Discover or `flatpak update`; **notify** = availability, changelog, and consent actions; **auto** = automatic update status with no consent actions. The OSD itself is constructed whenever CDP navigation is enabled, independent of all three modes. | `UpdatePromptController` is currently constructed only for `notify`; an always-present Settings surface cannot inherit that conditional lifetime. `UpdateState` has no "latest verified" signal, so a false availability bit must not claim that the installed version is latest. |
| O16 | The shared controls-row builder excludes the legacy config `show_controls` row and appends the fixed `Menu (☰) — Settings` row. The first-run card and Keys sub-tab both use that same builder. | The screen must describe the control that actually opens Settings, regardless of `first_run_overlay` or future user remaps. |

## 4. Architecture

Three cooperating pieces, mirroring the update/onboarding pattern exactly:

```
                      opens/closes, forwards commands            reads live state
  ┌──────────────┐   ┌───────────────────────┐   ┌──────────────────────────────┐
  │ input.cpp    │──▶│ OsdMenuController (C++)│──▶│ Config / Keymaps / LayerState│
  │ handle_osd() │   │  - lifecycle + model    │   │ UpdateCore snapshot (fold)   │
  │ (modal, top) │◀──│  - command(cmd)->reply  │   └──────────────────────────────┘
  └──────────────┘   │  - tick(on_watch)      │
        ▲            │  - on_page_reloaded()  │
        │ pad events └───────────┬───────────┘
        │                        │ ScriptLibrary::invoke_string gateway
        │                        ▼
        │            ┌───────────────────────────────────────────┐
        │            │ config/scripts/osd_menu.js  (JS component) │
        │            │  gateway({op, model/cmd}) -> JSON string;  │
        │            │  private window.__dbOSD owns focus/scroll  │
        │            │  CSP-safe: CSSOM + adoptedStyleSheets;      │
        └────────────┴───────────────────────────────────────────┘
                       Settings button: config/scripts/osd_button.js
```

### 4.1 C++ — `OsdMenuController` (new `launcher/src/osdmenu.{hpp,cpp}`)

Owns the menu's lifecycle and is the bridge between the input thread and the JS component. Modeled on
`UpdatePromptController`:

- `std::atomic<Lifecycle> lifecycle_{Closed}` where `Lifecycle` is exactly `Closed`, `Opening`,
  `Visible`, or `Reloading`. `GamepadInput` captures only in `Visible`; `Opening`/`Reloading` are
  deliberately non-modal. `open_from_button()` stores `Opening`, invokes the gateway with `op:"open"`,
  and stores `Visible` **only** after a valid reply says `present:true`; any error, malformed reply, or
  `present:false` stores `Closed`. `close()` stores `Closed` before best-effort `op:"close"` teardown.
- `OsdReply command(Op, params)` — the per-keypress bridge is a new
  **`ScriptLibrary::invoke_string(client, "osd_menu", params)`** helper (render + `eval_string`; today's
  `invoke` is `eval_void`-only). `osd_menu.js` receives the op and returns one JSON *string*:
  `{"present":bool,"generation":number,"verdict":string,"focus":{"tab":...,"subtab":...,"item":...}}`.
  `OsdReply` parses that fixed schema; it is not a general JSON API. The `OsdModel` serializer builds
  the nested model through a new typed `ScriptParams::set_json()` value, escaping all strings in one
  place; OSD call sites never use `set_raw` or concatenate JS. Empty, malformed, or `present:false`
  replies store `Closed` before the next input event. Valid replies update the saved focus and dispatch
  action verdicts (`update.confirm`, `update.ignore`) to UpdateCore. Note one eval per command is
  cheaper than normal navigation — `dispatch_key` is two CDP round trips per arrow
  (`devtools.cpp:744-748`).
- `void tick(bool on_watch)` — reconciles the **Settings button** (draw when not on watch; hide over
  playback, exactly like the old pill) and the **update badge** on it, strictly **edge-detected**
  like the pill's `want_dot`/`dot_shown_` pattern (`updateprompt.cpp:397-401`): CDP only on a state
  transition, never per tick. Also the bug-b guard, **layered so it is never a steady poll** (O12):
  1. **Closed menu (the steady state): zero CDP.** `tick()` is atomic loads plus the edge-detected
     button/badge draws. This is the battery contract.
  2. **Every command reply is a presence proof.** An empty/error/absent response changes `Visible` to
     `Closed` before the next event is handled. A navigation can race an already-arrived event, but it
     can consume at most that detecting event — it cannot leave a persistent invisible modal capture.
  3. **The explicit `state` command runs only while `Visible` AND no command has completed in
     the last ~1 s**, piggybacked on the input loop's existing idle wake (`input.cpp:446`). It exists
     to catch a body swap while the user isn't pressing anything — nothing else. In-page body swaps
     are normally self-healed by the keep-alive observer with no C++ involvement at all.
- `void on_page_reloaded()` — navigator thread. It first exchanges `Visible` for `Reloading`, making
  input non-modal, then takes `state_mu_` to snapshot the last valid focus and immutable model inputs.
  It re-injects with an **own** `DevToolsClient`, and publishes `Visible` only on a valid `present:true`
  reply; otherwise it publishes `Closed`. `state_mu_` protects the focus/model snapshot and is the only
  cross-thread state; the input client's DOM calls remain input-thread-only. The keep-alive observer
  handles in-page body swaps; this path handles full navigations.
- Holds a pointer to the folded **UpdateCore**, which exposes a thread-safe immutable snapshot
  (policy, availability, changelog, marker state) plus input-thread action methods. The Updates tab
  and the button badge therefore share one source of truth without sharing mutable controller fields.

### 4.2 JS — `config/scripts/osd_menu.js` (the focus-owning component)

A single parenthesised **gateway expression** invoked by `ScriptLibrary`, never by hand-built JS.
On each call it lazily installs a private `window.__dbOSD` component, then dispatches `p.op`:

- `open(model)` — build the overlay DOM from the model, set initial focus, and register the shared
  keep-alive (`window.__dbKeepAlive`) so a Leanback body swap cannot detach it. It is idempotent
  (rebuilds `#__deckback_osd` if present).
- `exec(cmd)` — apply one command to the focus/tab/scroll state. This is where roving-tabindex focus
  movement, `scrollIntoView`, sub-tab traversal, tab switching, and widget activation live.
- `close()` — drop from keep-alive (`window.__dbDropAlive`) first, then remove the node.
- `state` — report the current focus without mutating it.

Every op returns `JSON.stringify({present, generation, verdict, focus})`; `present` is false if the
component, its root, or the required shared prelude is absent. `focus` is `{tab, subtab, item}` only
when present, where `item` is the stable focused-item ID (or `"tab-strip"`). The gateway echoes the
model generation supplied by C++; C++ accepts a reply only when it matches its current generation.
This explicit string envelope is the wire format in O13: it fits `eval_string`, and a C++ reply parser
can reject a malformed or stale response instead of mistaking it for a visible overlay.

CSP-safe rendering, non-negotiable (from `self-update.md` ★ RENDER FIX): **no inline `style`
attribute, no `<style>` element.** Descendant rules go in a constructable `CSSStyleSheet` added to
`document.adoptedStyleSheets` (memoised `window.__dbOsdSheet`); dynamic per-node styling uses
`el.style.setProperty`. `innerHTML` (for the Keys table / changelog) goes through the memoised
Trusted Types policy `window.__dbTTP`; all untrusted text (changelog from GitHub) is HTML-escaped
first, exactly as `update_card.js` does. The keep-alive, `__dbTTP`, escape, and sheet helpers come
from the shared prelude `db_common.js` (O11, §4.4) — no private copies. The script keeps a defensive
existence check (prelude missing → draw nothing, never throw).

**Performance rules (O12) — requirements, not suggestions:**

- The overlay DOM is built **once per `open()`**. `exec()` moves focus by toggling a
  class/attribute on the two affected nodes; it never rebuilds a panel or re-renders a list.
- **No timers and no `requestAnimationFrame` loops while idle.** The only standing machinery is the
  passive keep-alive MutationObserver from the prelude.
- **Instant scrolling only**: `scrollIntoView({block:'nearest'})` with default behavior and plain
  `scrollBy` steps — never `behavior:'smooth'`, which animates on the compositor for hundreds of ms
  per keypress.
- **No `backdrop-filter`** on the panel (per-frame GPU blur on the 800p APU). Flat translucent
  background, like the update card.
- CSS transitions are allowed on open/close only, never on focus movement.

### 4.3 JS — `config/scripts/osd_button.js` (the Settings button)

The persistent top-right button: a pill with the gear/menu glyph and the label "Settings", plus an
optional amber **update badge** dot when an update is available. `pointer-events:none` (touch is inert
anyway under `disable_touch`; the button is activated by the controller, not a tap — see §6). It is one
normal one-shot script accepting `{visible, badge}`: `tick()` invokes it only when either desired value
changes, and the script idempotently creates/updates/removes `#__deckback_settings_button`. CSSOM
styling + keep-alive come from the shared prelude (O11), in the same role as today's `update_badge.js`
(which this replaces). The button is static: no animation or timers; a visibility/badge edge is the
only DOM change it ever makes.

### 4.4 JS — `config/scripts/db_common.js` (shared overlay prelude, O11)

Installed by `Navigator` **before its first app navigation** as a sticky document-start script
(`ScriptLibrary::install_sticky`, the same mechanism as `no_pointer.js`), so it survives target
recreation and exists before any overlay injects. It lazily creates its observer when the first node
is registered, so document-start timing never requires a ready body. Provides what every overlay
currently copy-pastes: `window.__dbKeepAlive` /
`__dbDropAlive` (the MutationObserver registry from `update_card.js:18-42`), the memoised Trusted
Types policy `__dbTTP`, the HTML-escape helper, and a memoised constructable-sheet helper. Overlay
scripts check for it defensively and no-op if absent. The `scripts_test.cpp` keep-alive/TTP guards
move here; `overlay.js` (first-run card) migrates onto it in the same change (§11).

## 5. Tab & content model (v1)

```
┌─ Settings ───────────────────────────  Updates ─┐   ← tab strip (L1/R1 or Menu switch)
│                                                   │
│  ┌ Keys ┐                                         │   ← Settings sub-tab rail (v1: one entry)
│  │ ●    │   Control            Action             │
│  │      │   ──────────────────────────────        │
│  │ (fut)│   A                  Select             │   ← read-only rows, derived from live keymap
│  │      │   B                  Back               │      (onboarding::controls_overlay_rows reuse)
│  │      │   X                  Play / pause        │
│  │      │   L2 / R2            Chapter back/fwd    │
│  │      │   Menu (☰)           Settings            │
│  │      │   D-pad / L-stick    Move focus          │
│  │      │   Right stick        Fast scroll         │
│  └──────┘                                          │
└───────────────────────────────────────────────────┘
```

- **Settings ▸ Keys** — a shared controls-row builder produces the `[control, action]` rows from the
  live keymap, dropping controls that resolve to no key and voice when disabled. It deliberately
  removes the legacy config `show_controls` row, then appends the fixed `Menu (☰) — Settings` row.
  The first-run card and Keys sub-tab use this exact builder, so the screen cannot teach an opening
  control that no longer works. Rows are **read-only** (not focusable in v1 — see focus priority §7).
  This satisfies "wire the data from real config; changes bound to the window" per O3: reopening the
  menu re-derives. **The row builder must count launcher-performed actions with no DOM key as live**
  — `voice_search`, `show_controls`, and the `chapter_*`/`skip_*` seeks (LT/RT). An early version
  dropped LT/RT because only the first two were whitelisted (`is_launcher_action`), so the shipped
  chapter-seek feature was invisible on the Keys tab; `onboarding_test.cpp` now asserts the real
  app.json produces the *Previous/Next chapter* rows.
- **About** (top-level tab). Title, summary, description, feature bullets, version, author, and
  project/support links, **single-sourced from the AppStream metainfo**
  (`flatpak/assets/io.github.properrr.deckback.metainfo.xml`) — the Flathub listing the app already
  ships, so the in-app page can never drift from the store page. `launcher/src/about.cpp` reads the
  installed `/app/share/metainfo/…` at startup and parses it (pure `parse_metainfo`, L0-tested); the
  version comes from the compiled-in `VERSION`, not the XML. Scrollable (reuses the transform scroll).
  The tab renders only when the launcher supplies `about_name`, so it is absent in the JS-only
  component tests. Future long-form product info (credits, licenses) extends this tab, not a new one.
  The top-level tab order is a list (`settings, updates[, about]`) that L1/R1 cycle, so adding tabs
  is a list entry, not a rework. (A temporary dev-only **Sandbox** tab of placeholder text once
  proved the transform scroll on hardware; it was removed once the About tab exercised the same
  scroll region with real content.)
- **Updates** has a policy-specific model:
  - **off:** `"Self-updates are off. Update Deckback from Discover or flatpak update."` No badge,
    changelog, or action is rendered.
  - **notify, no availability:** `"No update is currently available."` No action is rendered. This
    is deliberately not a claim that a remote check established "latest."
  - **notify, available:** `"v{available} is available. You have v{local}."`, a scrollable changelog
    (falling back to the releases link on fetch failure), and focusable **Update now** and
    **Ignore version** buttons. B is the non-focusable **Not now** action: it closes the menu and
    leaves the badge. Y ignores the version directly; both Y and the focused Ignore button call
    `ignore_version()`.
  - **notify, available but version not yet known:** `"A newer version is available. You have
    v{local}."` The portal sees a newer commit independently of the GitHub changelog fetch, which
    can lag, fail, or be absent (no-curl build), so `available` is momentarily/persistently empty
    while `has_update` is true. The action buttons still render. The status line MUST NOT collapse to
    "you're on the latest version" here — `osd_status_line()` takes `has_update` precisely to keep
    these two `available==""` cases apart (the refactor that moved the text off `draw_card()` lost
    this once; see osdmenu_test's three-state cover).
  - **auto:** `"Automatic updates are enabled."`; if the updater has published an available commit,
    append `"It will apply on the next launch."` There is no consent action or changelog fetch.

Future Settings sub-tabs (General, Input, Playback, About, …) are added as entries in the sub-tab rail
with their own widget lists; the shell does not change. **Integration rule (§13): every new
user-facing feature that has state or a setting adds a row/sub-tab here, not a new top-level
overlay.**

## 6. The Settings button — always-on, playback-gated

- Drawn by `OsdMenuController::tick(on_watch)`: **shown when `!on_watch`, hidden while a video
  plays** — the identical rule and signal (`LayerState::video_up()`) the update pill used, so it
  never sits over playback and returns on browse. Two accepted timing windows (≤ one player-poll,
  ≤1–2 s after a full reload) carry over verbatim from `self-update.md`.
- **Activation is by controller, not tap.** With `disable_touch` on, `no_pointer.js` swallows all
  pointer events, so the button cannot be clicked. Outside playback, the physical Menu/Start evdev
  control (`control_code("start")`) opens the OSD regardless of the keymap or first-run-card setting.
  The button is a visible affordance/label, not the input path.
- The **update badge** (amber dot on the button) replaces the standalone pill: it appears only for an
  unignored available update in **notify** mode and only when `!on_watch`, drawing attention to the
  Updates tab. It never appears in `off` or `auto` mode.

## 7. Focus model, widget types, and focus priority

The user asked for "proper focus priority by active elements: checkbox, combobox(list), regular
button, etc." Concretely:

**Focusable element types** (roving tabindex in `osd_menu.js`):

| Type | Focusable v1 | Activate (A) | Left/Right | Notes |
|------|-------------|--------------|-----------|-------|
| Tab (strip) | via L1/R1 or tab_next/prev, not the focus ring | switch tab | switch tab | Always reachable; not part of vertical focus order |
| SubTab (rail) | yes | focus its panel | — | Settings only; v1 has one |
| Button | yes | fire action verdict | — | Updates actions |
| Checkbox | yes (built, unused) | toggle → action verdict | — | For future writable settings |
| Combobox/Select | yes (built, unused) | open/commit | change value | For future writable settings |
| ReadOnlyRow | **no** | — | — | Keys rows: displayed, never focused |

**Focus priority / order** — deterministic, and this *is* the "priority" the spec asks for:

1. Vertical focus (D-pad up/down, left stick) visits focusable elements in **DOM order**, skipping
   `ReadOnlyRow` and any disabled widget. No focus trap, no dead end (input-ux §17 rule: "every
   direction leads somewhere").
2. **Initial focus** on open, and on entering a tab/sub-tab, lands on the **first enabled interactive
   widget** in the active panel (Button > Checkbox > Combobox are equal-priority; order is DOM
   order). If a panel has no interactive widget (e.g. Keys, Updates-off, or notify-with-no-availability), focus rests on the
   tab strip so B/back and tab-switch still work and nothing is stranded.
3. Left/Right is reserved for **tab switch** at the strip and **value change** on a focused
   Combobox; it never moves vertical focus. Horizontal-from-nowhere is a no-op verdict (`consumed`),
   never a fall-through to Leanback (we are modal).
4. Scroll: when the focused element is inside a scrollable region and focus moves past the viewport,
   `scrollIntoView` keeps it visible; the right stick scrolls the focused scroll region directly
   (§8).

## 8. Scroll

- **★ Native scroll does not work inside Leanback (verified on-Deck 2026-07-15).** youtube.com/tv
  resets `element.scrollTop` back to 0 asynchronously (~within 200 ms) on **any** element we inject —
  proven with a standalone scroll `<div>` that has nothing to do with the OSD. So a scroll region
  **cannot** rely on `overflow:auto` + `scrollTop`/`scrollBy`. Instead `.scroll` is a clip viewport
  (`overflow:hidden`, bounded height) holding a `.sinner` wrapper that osd.js translates itself
  (`transform: translateY(-off)`), tracking `box.__off` in JS and drawing its own `.sbar`/`.sthumb`
  thumb. Leanback cannot reset a transform we own. This is the mechanism the changelog and every
  future long-content tab must use. (Corollary: `ensureSheet()` must `replaceSync(RULES)` on every
  open, not once — a cached constructable sheet from an older osd.js pins stale CSS.)
- **D-pad / left stick**: move focus; keep the focused row in view. This alone makes every panel
  navigable.
- **Right stick**: analog fast-scroll of the **focused scroll region** (the changelog, a long
  settings list). C++ already reads the right stick raw and computes an analog repeat rate
  (`fast_scroll()` in `keymap.cpp`). While the OSD is open, that tick is redirected: instead of
  dispatching an arrow to Leanback, it calls `exec("scroll_up"/"scroll_down")`, and `osd_menu.js`
  does `region.scrollBy(0, ±step)` through the same O13 command gateway. This reuses the existing
  analog ramp, so the feel matches the rest of the app.
  **Rate cap (O12):** the redirected tick is capped at **~30 Hz** — the `fast_scroll` floor is 25 ms
  (`keymap.cpp:16`), i.e. 40 evals/s uncapped — and the pixel step scales with the actual interval,
  so full deflection means bigger steps, not more CDP round trips. Even uncapped this is cheaper
  than today's fast-scroll (one eval vs two `dispatch_key` round trips per arrow), but fewer,
  larger steps gives the same feel for fewer wakeups.
- **Right trackpad circular scroll** (the user's "swipe by circle clockwise → native scroll
  down/up"): a **follow-up pending an on-Deck spike** (O8). Open questions the spike answers: does the
  Deck's right trackpad reach our evdev layer at all (Steam Input may keep it as a mouse/wheel we
  don't read), or must `steam_input.vdf` map it to `REL_WHEEL` / a directional pad we can read? If it
  emits wheel events, note `no_pointer.js` does **not** swallow `wheel` (only pointer/mouse/touch), so
  wheel could reach a scrollable region directly — but that path is unverified. Design keeps
  right-stick scroll as the shipped mechanism and treats the trackpad as an enhancement with a clean
  seam (`exec("scroll_*")` is the single entry point either input drives).

## 9. Input capture, precedence, and the Menu button rework

**Precedence in `input.cpp::handle_event`** — the OSD handler goes **first** (O7):

```cpp
void GamepadInput::handle_event(int type, int code, int value) {
  if (handle_osd(type, code, value)) return;          // NEW: modal, wins over everything
  if (handle_onboarding(type, code, value)) return;
  ...
}
```

`handle_osd`:

- If `lifecycle != Visible` → return false (fall through to normal input), **except** it owns the
  fixed open edge: a press of `control_code("start")`, only while `!video_up`, calls
  `open_from_button()` and returns true. The open edge **defers while the first-run card is visible**:
  `handle_onboarding` dismisses that card on any button press, so the next Menu press opens Settings.
  `Opening` and `Reloading` deliberately return false for all other input — the overlay is not yet
  painted, so it must not be modal. Over playback Menu keeps its normal meaning (or nothing); the OSD
  does not open over a video and the button is not shown. L0-tested (§12).
- If `lifecycle == Visible` → **modal**: map the fixed Deck buttons to commands and swallow everything
  else.
  - D-pad/left stick → `up/down/left/right` (press edge + the existing auto-repeat, so held
    navigation works and accelerates). **Both dispatch sites route through one redirect function**:
    repeats mature in `loop()` (`input.cpp:498-504`), not in `handle_event` — if only the press edge
    were redirected, held-repeat arrows would leak to Leanback behind the modal menu, a new bug-b
    sibling. §12 L2 test 7 pins this.
  - **A** (`control_code("a")`) → `select`.
  - **B** (`control_code("b")`) → `back`: close a Combobox, else leave a sub-tab, else close the menu.
    At the Updates top level this is the explicit **Not now** action: it closes the OSD and leaves an
    available update badge in place.
  - **Y** (`control_code("y")`) → `ignore`. JS returns `action:update.ignore` only when the active
    tab is Updates and a notify-mode update is available; otherwise it returns `consumed`. This keeps
    Y from ignoring an update while the user is on Keys.
  - **L1/R1** (or the Menu button) → `tab_prev`/`tab_next`.
  - Right stick → `scroll_up`/`scroll_down` (analog, from the fast-scroll tick).
  - Everything else → swallowed (`return true`). No event reaches Leanback while the OSD is up.
- The reply's `verdict` tells C++ whether an action fired; `back` at the top level returns `close`
  and C++ calls `close()`. Any absent/malformed reply has already made the lifecycle `Closed`, so the
  next input event cannot remain trapped.

**Bug-b guard (the important part):** capture is tied to the last successfully confirmed `Visible`
state, never to an optimistic open flag. Each command is a presence proof; an error/absent reply
immediately drops capture, and the idle-only `state` command catches an in-page loss without polling.
`on_page_reloaded()` publishes `Reloading` before re-injection, so its known window is non-modal. A DOM
navigation can race one already-arrived input event, but it can never create the old persistent state
where every later key is swallowed behind an absent card. This is the exact defect from
`self-update.md` BUG 2b, designed out without violating O12.

**Menu button / controls card:** today `start` → `show_controls` re-opens the onboarding card, and the
update path piggybacks on it. In the OSD world:

- Menu (☰) is the fixed `control_code("start")` entry to the **OSD**, never the bare controls card.
  The legacy `show_controls` mapping is removed from every input map before normal dispatch; it is
  retained in old `app.json` files only as harmless backward-compatible data, not an OSD dependency.
- The **first-run controls card stays** as a one-time teaching overlay. It is no longer manually
  reopened; after first run, its content lives in Keys. Its shared row builder teaches Menu = Settings.
- `open_from_menu()`'s `return false` dead-end is **deleted** — Menu always opens an OSD with at least
  Keys plus an explicit Updates-policy state. Bug b cannot recur through a no-content path.

## 10. C++ ↔ JS command protocol

One narrow, executable protocol. C++ never re-renders the panel per keypress (O2); it invokes the
same embedded `osd_menu` gateway for every operation and parses one fixed reply schema.

**Request** — `ScriptLibrary::invoke_string(client, "osd_menu", params)` renders exactly one normal
script invocation, with these fields:

| `op` | Additional fields | Meaning |
|------|-------------------|---------|
| `open` | `model` (`ScriptParams::set_json`, from `OsdModel`) | build/rebuild the OSD and restore the requested focus |
| `exec` | `cmd` | process one navigation/action command |
| `close` | — | remove the OSD and keep-alive registration |
| `state` | — | presence/focus probe, used only by the idle guard |

`exec.cmd` is one of `up`, `down`, `left`, `right`, `select`, `back`, `ignore`, `tab_next`,
`tab_prev`, `scroll_up`, or `scroll_down`. There is no C++ expression such as
`window.__dbOSD.exec(...)`; only the gateway script knows the page-global implementation detail.

**Reply** — every successful JS invocation returns a JSON **string** with this exact shape:

```json
{"present":true,"generation":42,"verdict":"consumed","focus":{"tab":"settings","subtab":"keys","item":"tab-strip"}}
```

`present` is mandatory. When it is false, the remaining fields are ignored. When it is true,
`generation` and `verdict` are mandatory and `focus` contains only string `tab`, `subtab`, and `item`
fields. C++ accepts a reply only if its generation matches the currently rendered `OsdModel`; it then
authorizes an action against the active stable item ID. The small `OsdReply` parser rejects missing
fields, unknown types, malformed JSON, stale generations, and transport errors as `present:false`; it
is L0-tested with hostile strings. This is intentionally a fixed-schema parser, not a second
general-purpose JSON stack.

| Verdict | Meaning | C++ reaction |
|---------|---------|--------------|
| `consumed` | focus moved / scrolled / tab switched / intentional no-op | retain `Visible` and saved focus |
| `close` | `back` at top level | call `close()`, dropping capture before teardown |
| `action:update.confirm` | focused **Update now** fired | call UpdateCore `confirm_update()`, then close |
| `action:update.ignore` | focused **Ignore version** or contextual Y fired | call UpdateCore `ignore_version()`, then close |

Future writable settings add explicit action IDs such as `action:set.self_update_mode=auto`; v1 does
not emit them. `update.dismiss` and `controls.show` are intentionally absent: B is the close/Not-now
semantic and first-run controls are not a re-openable overlay.

`ScriptLibrary::invoke_string`, `ScriptParams::set_json`, `OsdModel` serialization, and the
fixed-schema `OsdReply` parser are all new L0-covered helpers. All untrusted text is escaped by the
model serializer before it becomes JSON; OSD call sites do not use `set_raw` or construct JS strings.

## 11. Migration & reconciliation

**Folding the self-update UI:**

- `update_badge.js` / `update_badge_hide.js` → **removed**; the badge becomes a dot on the Settings
  button (`osd_button.js`).
- `update_card.js` / `update_card_hide.js` → **removed as a standalone modal**; the card's content
  (heading, version, notes, A/B/Y) becomes the **Updates tab**. Its colour-coded glyph styling and
  Trusted-Types/escape discipline move into `osd_menu.js`.
- `UpdatePromptController` → **UpdateCore**: detection/changelog/consent plus a thread-safe snapshot
  for `OsdMenuController`; all standalone-overlay drawing is removed. The pure helpers
  (`compare_versions`, `parse_github_releases`, `notes_to_plain`, `summarize_releases`, marker I/O)
  remain unit-tested. `decide_notification` is replaced at its call site with the simpler
  notify-only badge decision — no v1 path auto-shows a card.
- `input.cpp::handle_update_card` → **removed**. Updates actions are `A` on focused Update now / Ignore
  buttons or contextual `Y` for Ignore; `B` is Not now by closing the OSD.
- `update_card_shown_v1` is **retired**: v1 never reads or writes it because it no longer auto-opens a
  card. `update_dot_dismissed_v1` remains the per-version Ignore marker used by the notify-mode badge.
  **Decision: v1 does NOT auto-open the menu; the badge is the passive signal, consistent with
  "notify, don't enforce."**
- `main.cpp` constructs `OsdMenuController` whenever CDP navigation is enabled. It constructs
  UpdateCore with the configured policy: absent updater for `off`, notify consent wiring for `notify`,
  and automatic status wiring for `auto`. `DECKBACK_FAKE_UPDATE` remains a **notify-mode-only** test
  fixture; no test infers that an `off` or `auto` policy offers consent.
- `overlay.js` (first-run controls card) **migrates onto `db_common.js` + adoptedStyleSheets in the
  same change.** Today it styles itself with an inline `<style>` element inside its innerHTML
  (`overlay.js:8-17`) — the exact pattern the RENDER-FIX finding says Leanback's CSP drops, and the
  pattern the L0 guard already *forbids* for `update_card`. Either that path actually paints on
  Leanback (then the guard is over-strict) or the first-run card has been shipping partially
  unstyled (TEST-PLAN §2B indeed marks its on-Leanback rendering unproven). Before the Keys tab is
  built on this template, verify on-Deck which it is and register a finding (§15); the OSD
  standardizes on adoptedStyleSheets + `setProperty` regardless, and the L0 CSP guard extends to
  `overlay.js`.

**Supersession of the companion configurator (`configurator-plan.md`) — O10.**

The OSD replaces the planned separate "Deckback Settings" app. The configurator's decisions that are
still correct are migrated here as the **writable-settings roadmap** (post-v1; v1 is read-only per
O4). Carried forward verbatim in intent:

- **Sparse `user.json` overlay** (was configurator C2): user settings live in
  `$XDG_CONFIG_HOME/deckback/user.json` holding **only keys the user changed**. The launcher loads
  shipped `app.json` first, then applies the overlay. "Reset to defaults" = delete the file; per-key
  reset = drop the key. Rationale unchanged: a full-copy user config would freeze the shipped
  defaults (UA, `cobalt_flags`) at fork time and break the R1 hot-swap lifeline; a sparse overlay
  keeps shipped defaults flowing through updates.
- **The R1/security keys are NOT user-editable** (was a configurator non-goal, and it is the more
  important rule now that editing is in-app): `url`, `user_agent`, `cobalt_flags`,
  `cdm_url`/`cdm_sha256`, `remote_debugging_port` stay owned by the *shipped* `app.json`. The OSD must
  never let a user setting **shadow an emergency UA/flag hotfix**. These keys have no OSD widget.
- **Button remap with conflict/lockout protection** (the configurator's headline): a remap sub-tab
  must refuse a binding that would strand the user — e.g. leave no control bound to `back`/`select`,
  or alter the fixed Menu/Start, D-pad, A/B/Y, and L1/R1 OSD controls (O5/O9 forbid that anyway).
  No per-direction D-pad remap and no rear-grip (L4/L5/R4/R5) remap — grips are duplicated to face
  buttons by Steam Input and are invisible to the evdev layer (configurator non-goals, still true).
- **Hardware-decode and touchscreen toggles** as *experimental, warned* switches (VA-API can regress
  on a Cobalt bump — CLAUDE.md; `disable_touch` default is load-bearing). These are checkbox/combobox
  widgets when they land.
- **In-app live-apply where safe** — the one thing the separate app could not do: because `input.cpp`
  holds the resolved `Keymaps` in-process, a keymap remap can be applied **live** (rebuild the maps)
  in addition to persisting to `user.json`; toggles that only the engine reads (hw decode,
  `cobalt_flags`-adjacent) still apply on next launch, and the menu says so. Decide per-setting.

Naming: the OSD button is labelled **"Settings"**; the superseded app's id
`io.github.properrr.deckback.Settings` is not built. No naming collision to document once the
companion app is dropped.

## 12. Test plan

New features get a **failing on-device test first** (TEST-PLAN §0). Because L2 (on-Deck automated)
now exists (`just test-deck`, first ran 2026-07-10), the OSD gets real coverage, not just unit tests.

### L0 (pure, `launcher/tests/`) — no engine

Reality check that shapes this tier: **there is no JS engine anywhere in the test stack.**
`scripts_test.cpp` asserts on script *text* (substring checks); it never executes JS. The focus
model therefore cannot be unit-tested at L0 — it is *executed* at L1 (below). L0 owns the C++ and
the string-level guards:

- `OsdMenuController` lifecycle: `Opening` and `Reloading` never capture; only a valid
  `present:true` reply makes it `Visible`; `close`, malformed JSON, `present:false`, and transport
  error make it `Closed`. `action:update.confirm` calls `request_update()` and
  `action:update.ignore` writes the Ignore marker; both close first.
- **Bug-b reconcile against `fake_cdp_server.hpp`**: an idle `state` reply that reports absent — or an
  `exec` reply that is absent, malformed, or errors — drops `Visible` to `Closed` before the next
  event. A navigator-thread reload transitions through non-modal `Reloading`, snapshots focus under
  the controller mutex, and publishes `Visible` only after successful re-injection.
- Gateway contract: `ScriptLibrary::invoke_string` renders the named `osd_menu` script invocation;
  `ScriptParams::set_json` serializes a nested `OsdModel`; `OsdReply` accepts only the §10 schema and
  rejects hostile strings. There is no direct `window.__dbOSD` expression in C++.
- Model builder: Keys rows use the shared builder (including fixed Menu = Settings) for a given
  keymap. Updates models cover `off`, `notify/no availability`, `notify/available`, and `auto`, and
  never emit a false "latest" claim.
- Open-edge interaction: a Menu press while the first-run card is visible dismisses that card and does
  **not** open the OSD; the next press opens it. With first-run disabled, the same fixed Menu press
  still opens Settings — it must not depend on an `OnboardingController` existing.
- `navigator_test` pins that `db_common` is installed before the first app navigation and re-armed on
  a reconnect; `scripts_test.cpp` covers the prelude itself.
- `scripts_test.cpp`: `db_common.js` carries the keep-alive observer + `__dbTTP` (guards move from
  the update scripts to the prelude); `osd_menu.js`/`osd_button.js`/**`overlay.js`** never use
  `setAttribute('style'`/`<style>` (the CSP guard now covers the first-run card too, §11); gateway
  close drops from keep-alive and every gateway branch returns the JSON-string envelope.

### L1 (headless engine, container) — the focus-model executable suite

**Hard deliverable, not an open question** (this resolves §15's old "testability off-Deck" item).
New `tests/smoke/test_osd_menu.py` (recipe `just test-osd`, wired into CI beside `just smoke`),
built entirely from existing parts: Xvfb + the engine as `smoke.sh` boots it, `scripts/cdp.py`
(`--navigate`, `evaluate`, `wait_for`), and a **local fixture page served with Leanback-equivalent
headers** (`cert.sh` already proves the container can serve local pages): CSP `style-src` without
`'unsafe-inline'` and `require-trusted-types-for 'script'`, so the CSP-safe rendering path is
*executed*, not just string-matched. `about:blank` is useless for this — it has no Trusted Types
(the `error_page.js` fallback exists for exactly that reason); the fixture must be a served page.

Install `db_common.js` at document start, then invoke the `osd_menu` gateway with fixture models and
drive `op:"exec"` through `Runtime.evaluate`. Assert the JSON-string reply and visible DOM for every
command from every state:

- initial focus lands on the first interactive widget; Keys panel (no widgets) rests focus on the
  tab strip;
- `down` past the last widget **clamps** at the boundary, never wraps or returns `consumed` with no
  focus;
- `left`/`right` switch tabs at the strip, change a combobox value when one is focused, and are a
  no-op elsewhere (never fall through);
- `back` closes a combobox, then leaves a sub-tab, then returns `close`;
- contextual `ignore` returns `action:update.ignore` only for an available notify update on the
  Updates tab; it is `consumed` from Keys and in every non-notify/non-available state;
- `tab_next`/`tab_prev` cycle Settings ↔ Updates and restore each tab's last focus;
- `scroll_*` moves the focused region and clamps at the ends;
- all four Updates policy models render their exact status: off/no availability/available/auto;
- **`getComputedStyle` assertions, not just node presence**: the panel is `position:fixed` with a
  non-degenerate rect, the focused row's highlight is applied, the changelog region is scrollable.
  DOM presence proves nothing about paint — silently dropped styling is this project's known
  failure class (★ RENDER FIX), and computed style catches it without a screenshot or a human;
- **Settings button coverage:** invoke `osd_button.js` too; assert its fixed placement/label, badge
  visibility in notify-available only, and its absence in off/auto/watch fixture states;
- keep-alive: swap/clear the body, assert the overlay re-attaches and gateway `state` still answers.

This suite is the review gate for the focus model: it runs on every push — no Deck, no Leanback,
no guest-account gate.

### L2 (on-Deck automated, `tests/deck/`) — the two-bug regression suite

The point of these is the two prior bugs. Written against the live gamescope session, DOM assertions
(not the unreliable gamescope screenshot — `self-update.md` caveat).

**Input-channel doctrine (binding):** every OSD open/navigate/close press is injected via **uinput**
(`tests/deck/lib/uinput.py`) — CDP `dispatch_key` bypasses `input.cpp` entirely and proves nothing
about `handle_osd`, the modal capture, or the open edge, which is what these tests exist for. CDP
is used **only to assert** DOM/state (`cdp.evaluate`), including the same `getComputedStyle` checks
as L1. One synthetic pad per module (fixture-scoped), not per test — pad creation costs a ~2.5 s
settle for the launcher's 2 s hotplug rescan.

1. **Open/close from the home screen.** Menu press on the YT home opens the OSD (`#__deckback_osd`
   present, `getBoundingClientRect` non-degenerate, focus on the tab strip for Keys). B closes it; the
   node is gone and **input is restored** (a subsequent D-pad press moves Leanback focus). Repeat with
   `first_run_overlay=false` to pin the fixed entry point. This is the direct guard for bug b.
2. **No input trap, ever.** While the OSD is open, assert the modal captures (a D-pad press does *not*
   move Leanback focus behind the menu). Then force a Leanback body swap / navigation and assert the
   menu either re-injects or becomes non-modal — after the reload's next settled input, **either** the
   menu is present **or** a following D-pad press moves Leanback focus. The test permits one detecting
   event during an asynchronous navigation but forbids a persistent invisible capture.
3. **Menu with nothing to offer.** With no update available (the common case), open the menu: it must
   show Keys + "No update is currently available", B/tab work, and A is consumed without reaching
   Leanback. B closes cleanly. The old "press Menu with no update → trapped, expected A/B/Y" scenario
   now resolves to a populated, navigable menu.
4. **Playback gating.** In a watch view (`location.hash` has `/watch`, `video_up`), the Settings
   button is hidden and Menu does not open the OSD over playback; back out to home and the button
   returns and Menu opens it. Guard against bug a's cousin: the menu must not draw over or act on
   playback state it shouldn't.
5. **Off-watch action safety (bug a class).** No OSD action may touch the player when not on a watch
   view. Assert opening/closing the menu and firing Updates actions from the home screen never calls
   `seekTo`/`play` on the backgrounded player (reuse the bug-a probe: previous video stays paused, no
   audio resumes).
6. **Notify-mode update round-trip through the tab.** With `DECKBACK_FAKE_UPDATE` and
   `self_update_mode=notify`, the Settings-button badge appears (off playback) and the Updates tab
   shows its changelog plus Update now / Ignore version. In **separate clean fake-update launches**,
   assert: A on Update now fires `request_update()` (journal-confirmed); B closes but leaves the badge;
   and Y on Updates persists the Ignore marker. This is the on-Deck notify round-trip that
   `self-update.md` still lists as the pending gate — now run through the menu.
   Needs a new fixture that **restarts the launcher on the Deck with `DECKBACK_FAKE_UPDATE` set**
   (the env var exists, `main.cpp:370-374`, but no deck test drives it today) — state-destroying
   but fully automatable.
7. **Held-repeat containment.** Hold D-pad down for ≥1 s while the OSD is open: OSD focus advances
   (accelerating), and Leanback's `activeElement` behind the menu never changes. This pins the
   second dispatch site — repeats mature in `loop()`, not `handle_event` — to the redirect (§9).
8. **Policy states.** Restart with `self_update_mode=off` and `auto`: Settings still opens; Updates
   shows the defined policy text; no notify badge, consent action, or fake-update round-trip is
   exposed. This pins O15 rather than treating notify as the only supported app configuration.

**Dependency to name honestly:** tests 4–5 need a real watch view, and L2 currently sits behind
Leanback's guest-account gate (TEST-PLAN §2). Plan on a deep-link to a known-good video as the
fixture. If playback cannot be reached unattended, those two cells — and only those — fall back to
the human pass, and the tests must exit **3** (environment), never quietly pass.

### States matrix to cover (from the spec: "all states, focuses, before/after menu calls")

before-open input works → open → focus states (each tab, each widget, scroll top/mid/bottom) →
tab-switch preserves focus → action fires → close → after-close input works; ×
{home screen, watch view} × {off, notify/no availability, notify/available, auto}. Every cell asserts
*no focus trap and no stranded capture*.

### The human gate (final stage only)

With L0 + L1 + L2 green, the only human step is **one visual pass on-Deck**: 10-foot readability,
button/badge placement and contrast, scroll feel — taste, not function. Every functional claim
above (presence, focus, modal capture, styling-actually-applied, playback gating, the update
round-trip) is machine-checked; a human never re-verifies those.

## 13. Integration guideline for future work (keep this current)

### 13.1 UI/UX standard — the OSD is a console menu, not a web page

Every future tab and sub-tab follows these rules. They are product requirements, not optional polish:

- **One stable place for each thing.** Top-level tabs are product domains (`Settings`, `Updates`), not
  implementation areas. Settings sub-tabs are stable categories (`General`, `Input`, `Playback`,
  `About`), ordered by user frequency. Add a top-level tab only when its domain warrants a persistent
  badge or has multiple sub-tabs; otherwise add a Settings sub-tab or section. Keep the initial
  hierarchy to two levels (tab → sub-tab/panel); a deeper flow must be a temporary detail view with a
  clear B-back route, not a permanent third rail.
- **D-pad location must be predictable.** L1/R1/Menu always change top-level tab; Up/Down follow the
  rendered focus order; Left/Right only changes a focused Choice or operates at the tab strip. Focus
  is preserved by stable item ID per tab, not a numeric index, when a model refreshes. At a list edge
  focus **clamps** and the command is consumed — it never wraps unexpectedly or leaks to Leanback.
  Entering a panel chooses its first enabled interactive item; returning from a detail view restores
  the invoking item. Every panel, including an empty/error/loading panel, has a B route and tab switch.
- **Make state visible without a tutorial.** A persistent title/breadcrumb identifies tab and sub-tab;
  selected tabs, current values, disabled reasons, restart requirements, and destructive consequences
  are textually stated. Colour reinforces state but never is its only signal. A newly available update
  uses the button badge; no panel may steal focus or auto-open while the user browses.
- **Design for the 10-foot, 800p display.** Use a 22 px minimum body size, 28 px minimum section
  heading, 44 px minimum logical row height, a visible focused-row outline plus background contrast,
  and at least 4.5:1 text contrast against its immediate background. Keep labels short, put explanatory
  text on a second line, cap a panel to one primary action, and keep the active tab/sub-tab plus a
  scroll-position cue visible while a long panel scrolls. Never rely on hover, pointer precision,
  smooth scrolling, or animation to communicate state.
- **Modal, but always escapable.** B closes a top-level panel/OSD and reverses transient Choice popups;
  no background input leaks while `Visible`, but `Opening`/`Reloading` remain non-modal (§4/§9). A
  successful action gives immediate in-menu feedback (updated value/status) and never a blocking toast
  that obscures the next control. External work (network, persistence, updater request) runs off the
  input thread and exposes a loading/error/retry row rather than freezing focus.

### 13.2 Reusable panel and item contract

Future features add **data to `OsdModel`**, not new injected overlays or one-off focus handlers. The
JS renderer owns all focus traversal and renders these common item kinds from stable IDs:

| Item kind | Purpose | Interaction contract |
|-----------|---------|----------------------|
| `ReadOnlyRow` | status, version, key mapping, explanatory text | never focusable; changes only through a model refresh |
| `ActionButton` | reversible one-shot action | A emits one namespaced action ID; stays disabled while pending |
| `Toggle` | boolean setting | A toggles; describe immediate vs restart-required effect |
| `Choice` | finite enum/combobox | A opens a temporary option list; Left/Right changes only the focused choice; B cancels an uncommitted list |
| `Section` | group heading/help | never focusable; no fake button styling |
| `DangerAction` | reset, delete, or disruptive operation | A opens an explicit confirmation detail view; B is the safe default; confirmation names the effect and any restart |

Every interactive item declares: stable `id`; visible `label`; optional `description`; `enabled` plus
a user-readable disabled reason; current `value`/options where applicable; action ID; `pending` state;
and `apply` (`immediate`, `restart-required`, or `confirmation-required`). Model construction omits
inapplicable items rather than rendering unexplained disabled controls. A panel may add custom copy,
but not a custom input path, CSS focus rule, or direct CDP callback.

Action IDs remain namespaced (`update.confirm`, `input.set_layout`, `general.reset_defaults`) and are
validated by the C++ dispatcher against the active item ID and current model generation. A stale reply
or duplicate input event therefore cannot apply an action after the panel changed. Writable settings
must update the model only after persistence succeeds; on failure they retain the prior value, show an
inline retryable error row, and preserve focus. Restart-required changes show `Applies next launch`
before confirmation, not after the user has already committed.

### 13.3 Required test coverage for every future panel

The feature owner adds table-driven cases to the existing OSD suites before calling a tab complete:

- **L0:** model serialization, action authorization/generation check, value validation, disabled and
  pending transitions, persistence success/failure (where applicable), and the exact restart/confirm
  policy. Include malformed/stale action replies so they fail closed.
- **L1:** every item kind gets focus entry, Up/Down boundary clamp, Left/Right behavior, A activation,
  B cancellation/back, disabled-state skip, model-refresh focus restoration, and computed-style checks
  for focus visibility, row size, contrast tokens, and scrollability. Assert semantic roles/attributes
  (`tablist`/`tab`/`tabpanel`, `aria-selected`, `aria-disabled`, control label) as well as paint.
- **L2:** use uinput for each new fixed control or action that passes through `input.cpp`; prove modal
  containment, close/input restoration, playback gating where relevant, and that a failure leaves no
  persisted partial setting. Add a real app state only when a local fixture cannot exercise it.
- **Human gate:** one 800p on-Deck pass checks legibility, focus discoverability, terminology, and
  scroll feel. It confirms taste; it never substitutes for the automated behavioural tests above.

Every new user-facing feature or setting **integrates into the OSD**, not as a new overlay:

- Read-only state → a row/section in an existing (or new) Settings sub-tab, derived from the live
  source, never hardcoded (Keys is the template).
- A user setting → a widget (checkbox/combobox/button) in a Settings sub-tab; persist to the
  `user.json` overlay (shared with the configurator); add a namespaced action ID and its §13.2 apply
  policy.
- A one-shot notification (like "update available") → a **badge on the Settings button** + content in
  the relevant tab, playback-gated via `video_up`. Not a new modal that pops itself.
- New Deck controls → a Keys row automatically (it derives from the keymap); the fixed Menu = Settings
  row remains launcher-owned and is never replaced by a keymap row.

## 14. File-by-file change list (implementation, when approved)

New:
- `launcher/src/osdmenu.hpp` / `.cpp` — `OsdMenuController` (§4.1) + pure helpers (model builder,
  `OsdReply` parser, lifecycle) for L0 tests.
- `config/scripts/osd_menu.js` — the O13 command gateway and focus-owning component (§4.2).
- `config/scripts/osd_button.js` — the Settings button + badge (§4.3).
- `config/scripts/db_common.js` — shared overlay prelude (O11, §4.4), installed sticky.
- `launcher/tests/osdmenu_test.cpp` — L0 (§12).
- `tests/smoke/test_osd_menu.py` + fixture page (CSP/Trusted-Types headers) + `scripts/test-osd.sh`
  + `just test-osd` recipe, wired into CI — L1 focus-model suite (§12).
- `tests/deck/test_osd.py` — L2 two-bug regression suite (§12), uinput-driven, incl. the
  fake-update restart fixture.

Changed:
- `launcher/src/input.hpp` / `.cpp` — add `handle_osd`, slot it first in `handle_event`; add
  `OsdMenuController*` to `GamepadOptions`; reserve physical Menu/Start with `control_code("start")`
  independently of onboarding/keymap; remove legacy `show_controls` before normal dispatch; route
  **both** direction-dispatch sites (press edge in `handle_event`, matured repeats in `loop()`) through
  one redirect only while `Visible`; redirect the right-stick tick to `scroll_*` (rate-capped, §8);
  update the fast-scroll predicate; and remove `handle_update_card` (folded).
- `launcher/src/scripts.hpp` / `.cpp` — add `invoke_string` and typed `set_json()` (§10).
- `launcher/src/navigator.hpp` / `.cpp` — install `db_common` before the first navigation and keep it
  sticky across reconnects (O14).
- `config/scripts/overlay.js` — migrate onto `db_common.js` + adoptedStyleSheets (§11); drop the
  inline `<style>`.
- `launcher/src/updateprompt.hpp` / `.cpp` — refactor to UpdateCore (policy snapshot + detection /
  consent) and remove standalone overlay drawing; retire the card-shown marker; keep relevant pure
  helpers.
- `launcher/src/main.cpp` — construct `OsdMenuController` whenever CDP navigation exists, wire it into
  input + navigator `on_app_loaded`, and configure UpdateCore for off/notify/auto; stop constructing
  standalone pill/card drawing.
- `launcher/src/onboarding.*` — extract the shared Keys-row builder; it appends fixed Menu = Settings
  and first-run controls stay one-time only.
- Remove: `config/scripts/update_badge.js`, `update_badge_hide.js`, `update_card.js`,
  `update_card_hide.js` (content folded into the OSD).
- `config/app.json` — remove the shipped `show_controls` mapping/comment and document Menu as a fixed
  launcher control. No new required OSD keys in v1. A future `osd` block (e.g. `auto_open_updates`) is
  optional and defaults off.
- Docs: `docs/HOW-IT-WORKS.md` / `docs/SUPPORT.md` — the Settings button and menu; `CHANGELOG.md`.
- `.internal/TASKS.md`, `.internal/TEST-PLAN.md` — OSD tasks + the two-bug L2 rows.

## 15. Open questions / risks

- **Right-trackpad scroll spike (O8):** exact input path on-Deck is unverified. Blocking? No —
  right-stick scroll ships; trackpad is additive.
- **JS focus component testability off-Deck: RESOLVED (2026-07-15 review).** There is no JS engine
  in the repo's test stack, so "unit test the component" was never available; the harness is the L1
  fixture suite (§12): headless engine + `cdp.py` + a served page with Leanback-equivalent
  CSP/Trusted-Types headers. It is a hard deliverable of v1, not an option to pick later.
- **Does `overlay.js`'s inline `<style>` actually paint on Leanback?** Unknown — the RENDER-FIX
  finding says CSP drops it; the card's "verified rendering" claim says *something* painted.
  Resolve on-Deck during the §11 migration and register a finding. The OSD does not depend on the
  answer (it standardizes on adoptedStyleSheets either way), but the L0 CSP guard's scope does.
- **Auto-open on new commit:** decided off for v1 (§11). Revisit if users miss the old auto-card — but
  the badge is the "notify, don't enforce" signal and enforcing an overlay over browsing is what we
  avoided.
- **First-run card vs OSD Keys overlap:** keep the first-run card (teaches the menu exists), or make
  first-run auto-open the OSD Keys tab once? v1 keeps the separate first-run card (lowest risk);
  revisit.
- **LCD (Van Gogh):** untested, as everything. The OSD is pure DOM + evdev, no APU dependency, so LCD
  risk is low but unproven.
- **Player Menu key:** if a real `player_menu` key is found on-Deck, bind Menu only in the player layer.
  OSD interception is already off-watch-only, so this adds player functionality without competing with
  Settings or changing the fixed browse entry point.
