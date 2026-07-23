# Exit / Quit — design doc

Status: **PROPOSED** — no code written. Scope: how a user deliberately leaves Deckback, where that
control lives, and how they find out it exists. Supersedes nothing; closes one open question in
`findings/durable/input-ux.md` (Back-at-root).

---

## 1. The actual problem

Deckback can already be exited. Steam Game Mode owns app lifecycle: **STEAM → Exit Game**, and
**STEAM + B (held)** force-quits. So this is *not* a capability gap, and any design that claims
"users are trapped" is wrong.

The real problems are narrower and worth fixing:

1. **Discoverability.** Deckback is a *non-Steam shortcut*. The exit path lives in Steam's UI, not
   ours, and nothing in the app hints at it. A TV-style app with no visible way out reads as broken
   even when it isn't.
2. **Force-quit is the path of least resistance, and it is the dirty one.** A user who does not find
   "Exit Game" holds STEAM + B. That is a kill: no pause, no position checkpoint, no orderly release
   of the logind inhibitors. The app already knows how to shut down cleanly — it just has no way to
   be *asked* from inside.
3. **We own a checkpoint the platform does not.** `PlayerController::on_suspend()` pauses and records
   the playback position. Exit should reuse it, so "resume where you left off" holds for a deliberate
   exit exactly as it does for sleep.

So the goal is **a clean, discoverable exit**, not a new capability. Framing it that way keeps the
design small and stops it from competing with Steam.

## 2. What the platform already gives us (do not rebuild)

| Path | Clean? | Keep |
|---|---|---|
| STEAM → Exit Game | yes — SIGTERM → our handler | yes, document it |
| STEAM + B (hold) | **no** — force kill | yes, as the panic button |
| Steam "Close game" | yes — same SIGTERM path | yes |

`main.cpp:46` wires `SIGINT`/`SIGTERM` → `Watchdog::request_shutdown()`. The watchdog sets
`g_shutdown`, and on child death returns 0 with `"shutdown requested"` — **explicitly not a crash and
not restarted** (`watchdog.cpp:75-82`). An in-app exit therefore does not need a new shutdown
mechanism; it needs to reach the mechanism that exists. That is the single most important constraint
here, and it makes this cheap.

## 3. Where to put it — options considered

| # | Option | Verdict |
|---|---|---|
| A | **Item in the OSD menu** (Menu ☰) | **CHOSEN** |
| B | Dedicated button (`Y` is free) | Rejected as primary — footgun |
| C | Back (B) at the Leanback root quits | Rejected — fragile |
| D | Steam only (status quo) | Kept as fallback, not sufficient alone |

**C — Back-at-root.** The Android TV idiom is that repeated Back exits. `input-ux.md` left this open:
*"swallow Back at the Leanback root, or quit."* Rejecting it: detecting "root" means reading
Leanback's own route, an undocumented contract this project has a standing rule against guessing
(R1, and the same reasoning that keeps `player_open` a layer hint rather than a truth). A false
positive quits the app mid-browse — the worst possible failure for a destructive action. This design
**closes that question as "swallow"**: Back never exits. With an explicit exit present, Back no longer
has to carry that load.

**B — a bare button.** A single stray press ending the session is unacceptable, and the keymap is
user-remappable *and* remotely hot-swappable, so a destructive action in it could land anywhere. Also
`controls_overlay_rows()` would then advertise it on the first-run card, teaching users a one-press
kill. If ever wanted, it must be hold-to-fire and opt-in; not in v1.

**A — the OSD.** Decision O10 already makes the OSD *the* single in-app surface, it is
keymap-independent (O9), couch-reachable by a fixed button, and it already models "a one-shot action
that closes the menu" (`OsdVerdict::Kind::Apply` vs `Kind::Action`, with `on_update_confirm`-style
callbacks in `OsdMenuConfig`). Exit is a natural fifth citizen there, and costs no new overlay.

### 3.1 Where *inside* the OSD

| Placement | Assessment |
|---|---|
| Last row of Settings ▸ some sub-tab | **No.** Exit is not a setting; Settings has sub-tabs, so it would be buried two levels down. |
| A new top-level "Exit" tab | **No.** Tabs present content. A tab that *is* an action breaks the model and sits in the L1/R1 cycle, where the user passes through it constantly. |
| Bottom of the **About** tab | Cheap, defensible (About is already app-meta), but nobody looking to quit opens "About". |
| **Persistent footer action, present on every tab** | **CHOSEN.** Not a setting, not a fake tab, always visible, one focus move from anywhere. |

Cost of the footer: the focus model lives in `osd.js` (O2), so "the last focus stop on every tab is
the footer" is a JS focus-traversal change plus a rendering change — contained, and unit-testable in
the L1 JS suite the OSD plan already requires. That cost is the price of the one property that
matters most here: **discoverability**. Burying the exit reintroduces the very problem we are solving.

## 4. Interaction design

```
┌─ Deckback ─────────────── Settings │ Updates │ About ─┐
│                                                        │
│   (tab content)                                        │
│                                                        │
├────────────────────────────────────────────────────────┤
│  ⏻  Exit Deckback                                      │   ← footer, all tabs
└────────────────────────────────────────────────────────┘
                A Select    B Back
```

On activate, the row expands **inline** — no modal, no new screen, no focus trap:

```
├────────────────────────────────────────────────────────┤
│  ⏻  Exit Deckback?  Your place is saved.               │
│         [ Cancel ]   [ Exit ]                          │
└────────────────────────────────────────────────────────┘
             ^ focused by default
```

- **Cancel is pre-focused.** The safe option must be the default on a terminal action; a user
  mashing A never exits by accident.
- **B cancels** and returns focus to the collapsed row — consistent with B = back everywhere else,
  and it does *not* close the whole menu from the expanded state (one predictable step).
- **No modal dialog.** The adopted TV checklist requires every direction to lead somewhere; an inline
  expansion keeps D-pad traversal intact.

**Why a confirm at all**, given the path is already Menu → navigate → A: because the cost asymmetry is
extreme. A wrong Cancel costs one press; a wrong Exit costs the session. One extra press on a rare
action is the right trade.

*Alternative considered:* **hold-A-to-confirm** with a progress fill ("Hold A to exit") — one
interaction, impossible to trigger accidentally, and the haptics exist (`haptic.cpp`). It is arguably
more elegant, but it is less conventional, is invisible until attempted, and needs animation plumbing.
Recommend inline confirm for v1; hold-to-confirm is a clean follow-up if the confirm step annoys.

**Wording.** "Exit Deckback", not "Quit"/"Close" — Steam Game Mode says *Exit Game*, so this matches
the platform vocabulary the user already learned, and naming the app disambiguates it from leaving a
video. "Your place is saved" sets the resume expectation, which is the whole reason to prefer this
over force-quit.

## 5. What Exit must actually do

Ordered, because the order is the feature:

1. **Pause + checkpoint** the video (reuse the `on_suspend()` checkpoint path) — this is what makes
   the promise in the confirm line true.
2. **Release** the idle inhibitor (the poll loop already does this on exit; do not leave a stale one).
3. Call **`Watchdog::request_shutdown()`** — sets `g_shutdown`, SIGTERMs the child.
4. Watchdog returns **0**, logs `shutdown requested`, and **does not restart** — the property that
   makes this safe (`watchdog.cpp:75-82`). Anything that skips step 3 and just kills the child would
   be restarted as a crash.
5. Process exits 0 → **Steam Game Mode**, back to the library. Never a blank shell — the destination
   `input-ux.md` requires.

The single-instance lock and the host keep-awake helper need no special handling: the lock releases on
process exit, and the helper stops nudging the moment our playback inhibitor disappears.

### 5.1 The one case that must be blocked

**An update deploy in flight.** The self-updater deploys through the Flatpak portal; killing the app
mid-deploy risks a half-applied update. The footer row must render **disabled** ("Finishing update…")
while `UpdateCore` reports a deploy in progress, and Exit must be refused rather than queued. This is
the only state where Exit is not immediately safe, and it is easy to miss.

## 6. How the user finds out

Ranked by value, cheapest first — and deliberately quiet, because this duplicates a platform
capability rather than adding a new one.

1. **The footer is self-revealing.** It is visible the first time the menu is opened, on every tab.
   For a control that duplicates STEAM → Exit Game, that is proportionate.
2. **The first-run controls card.** It already carries the fixed row `Menu (☰) — Settings` (O16).
   Change that row to **`Menu (☰) — Settings & Exit`**. This teaches the exit at the exact moment we
   already teach the Menu button, costs one string, and inherits the card's existing appearance rules.
   The same shared builder feeds **Settings ▸ Keys**, so both surfaces update together and cannot
   drift — the property O16 exists to guarantee.
3. **`docs/SUPPORT.md`** gains a short *Leaving the app* section: both routes, and that the in-app one
   saves your place while a held STEAM + B does not.
4. **No toast.** Toasts announce state changes the user caused. An unprompted "by the way, you can
   exit" is noise, and this project already treats unactionable prompts as a defect (the keep-awake
   warning stays silent when it cannot know).

Note the shared-builder subtlety: `controls_overlay_rows()` is *keymap-derived* and deliberately drops
controls that dispatch nothing. Exit is a menu item, not a keymap binding, so it will **not** appear
automatically — it rides in on the fixed Menu row (point 2). That is the correct place for it and
avoids implying a bindable Exit button exists.

## 7. Decisions

| # | Decision | Rationale |
|---|---|---|
| E1 | Exit lives in the **OSD menu**, as a **persistent footer action on every tab** | O10 makes the OSD the single surface; footer keeps a non-setting out of Settings and out of the tab strip while staying discoverable |
| E2 | **Inline two-step confirm**, Cancel pre-focused, B cancels | Terminal action, extreme cost asymmetry; inline avoids a modal/focus trap |
| E3 | **Not bindable** to a controller button in v1 | A destructive action inside a remappable, hot-swappable keymap is a footgun, and the first-run card would advertise a one-press kill |
| E4 | **Back never exits**; Back at the Leanback root is **swallowed** | Closes the open question in input-ux; root detection needs an undocumented Leanback contract, and a false positive is catastrophic |
| E5 | Exit **pauses + checkpoints** before shutting down | Makes "your place is saved" true and beats force-quit; the only user-visible reason to prefer this over STEAM + B |
| E6 | Exit routes through **`Watchdog::request_shutdown()`** | The only path the watchdog treats as success rather than a crash to restart |
| E7 | Exit is **refused while an update deploy is in flight** | A half-applied portal deploy is the one genuinely unsafe interruption |
| E8 | Label is **"Exit Deckback"** | Mirrors Steam's "Exit Game"; naming the app disambiguates from leaving a video |

## 8. Test plan

L0 (no device):
- `osdmenu_test`: a new verdict/action parses to the Exit action; the footer row is present for every
  tab; the row is disabled while a deploy is in flight (E7).
- JS L1 (the suite the OSD plan already owes): footer is the last focus stop on each tab; expanding
  focuses **Cancel**; B from the expanded state collapses rather than closing the menu (E2).
- `watchdog_test`: a shutdown request yields exit 0 and **no restart** — the E6 guarantee. This is the
  regression that would otherwise resurrect the app the user just closed.

L2 / on-device (the parts L0 cannot prove):
- Exit during playback → returns to the Steam library, no relaunch, and reopening resumes at the
  checkpointed position (E5).
- Exit is not misread as a crash: `watchdog: shutdown requested` in the log, no restart line.
- The first-run card and Settings ▸ Keys both read `Menu (☰) — Settings & Exit`.

## 9. Open questions

- **Confirm vs hold-to-confirm** (§4). Recommend inline confirm; revisit if it feels heavy in hand.
- **Should Exit also appear when the OSD is opened during playback?** The OSD gates some actions on
  playback. Exit should stay available — the most likely moment to want out is mid-video — but that
  interacts with the playback-gating rules and should be confirmed against `osdmenu.cpp`.
- **Power-off / sleep adjacency.** Deliberately out of scope: sleep is the STEAM button's job, and
  adding a second power verb next to Exit invites mis-selection.
