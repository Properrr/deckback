# Session-bus reconnect for the self-updater (launcher/src/updater.cpp)

## Status: IMPLEMENTED (2026-07-15) — L0 unit-tested (backoff/give-up), `just preflight` green, two independent reviews (design + implementation) incorporated. **On-Deck integration drive PENDING** (Deck was asleep): rebuild + `just deck-install-dev`, run the app, then `systemctl --user restart flatpak-portal` (case B) and a bus drop (case A) while watching `journalctl --user` for `flatpak-portal restarted — re-creating the update monitor` / `reconnected to the Flatpak portal`.

## Review incorporated (2026-07-15)

An independent review found two real hang/flake risks and one API bug; the design below is the
corrected version. Resolutions:

- **C1 — `stop()` must not block on a synchronous `sd_bus_call`.** The wake pipe cannot interrupt an
  in-flight `sd_bus_call` (each bounded by `kCallTimeoutUsec` = 5s). So `establish()` checks
  `running_` immediately before each blocking step and bails if a `stop()` landed; consent is seeded
  **once per session** (`seeded_`), so reconnects skip that call entirely. Worst-case join latency is
  therefore **one in-flight ~5s call** (the pre-existing startup worst case), documented, not the
  ~15s a naïve retry would add.
- **C2 — the backoff wait is `poll(wake_pipe_[0], delay)`, interruptible, with `running_` checked at
  the top of every retry iteration** (`establish_with_retry`'s `while (running_.load())` + the
  wait returning `running_`). This is also the gate that keeps the `updater_test.cpp` `start()/stop()`
  lifecycle fast: a stop nudge breaks the backoff at once.
- **S3 — the portal-owner match uses `sd_bus_add_match` with an explicit match string**
  (`type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.freedesktop.portal.Flatpak'`),
  NOT `sd_bus_match_signal` (which has no `arg0` filter and would wake on every session-bus name
  change). Arg order is `(name, old_owner, new_owner)`; act only when `new_owner` is non-empty.
- **S4 — a dedicated `slot_owner_`** is unref'd in teardown and recreated in `establish()`.
- **S5 — `want_update_` is NOT reset on a monitor rebuild** (only `updating_` is): a latched "Update
  now" consent survives a portal restart. `UpdateState.available_` is never cleared here.
- **S6 — no reconnect flap loop:** consent is re-seeded never (once per session), and the retry
  budget resets **only after a connection stayed up ≥ `kReconnectStableMs` (30s)** (monotonic clock),
  so a portal that connects-then-drops marches toward give-up instead of hammering every 1s.
- **S7 — case-A give-up `return`s from `loop()`** (the bus is torn down/null; continuing would
  `sd_bus_get_fd(nullptr)`).
- **N8 — `reestablish_monitor_` is a plain `bool`** (written by the `NameOwnerChanged` callback which
  runs on the loop thread inside `sd_bus_process`, read by the loop), cleared right after case B acts.
- **N9 — `reconnect_delay_ms` clamps the shift** (`1000ull << min(attempt, 6)`); L0 asserts added.
- **N10 — `teardown()` unrefs slots before the bus** (slots hold a bus ref).
- **N12 — `selftest_deploy` keeps its own single-attempt connection**, never routed through the retry
  path.

## Implementation review incorporated (2026-07-15)

A second review of the *written* code (build-clean, unit tests pass) found three real holes, now fixed:

- **SF1 — flap loop when `establish()` keeps succeeding but the link dies instantly.** The retry
  backoff only fired on an `establish()` *failure*; a connect-then-instant-drop cycle re-established
  every iteration with no `poll` wait and a never-climbing counter → a tight reconnect spin. Fixed:
  case A now treats a **non-stable** drop (link up < `kReconnectStableMs`) as a counted failure —
  `++reconnect_failures_`, `wait_backoff`, and give-up when the budget is spent — so a flapping portal
  backs off and eventually goes inert instead of spinning.
- **SF2 — a failed consent seed was latched.** `seed_update_permission()` set `seeded_` even when the
  `SetPermission` call itself failed (permission store unreachable at bring-up), so a later successful
  reconnect would build the monitor but never re-seed → auto-deploy silently blocked. Fixed:
  `seed_update_permission()` now returns `bool` (true only on a *determinate* outcome — not sandboxed,
  already yes/no, or a grant that succeeded); `establish()` latches `seeded_` only on true, so a
  transient seed failure re-seeds on the next reconnect.
- **SF3 — documented `stop()` bound.** `seed` issues two back-to-back 5 s `sd_bus_call`s; a
  `running_` check now sits between them (skip the grant after a stop), keeping the worst-case join at
  one in-flight ~5 s call as C1 claims.
- **N1 — `run()` sets `running_ = false` uniformly after `loop()`**, so every give-up/exit path leaves
  the same post-condition.

Verified-fine by the review (no change): reentrancy/thread-confinement of all callbacks, slot/bus
unref ordering + no double-free/leak across reconnect, the NameOwnerChanged arg0 filter + race-free
"subscribe after establish_monitor" ordering, `want_update_` preserved / `updating_` cleared on
rebuild, `selftest_deploy` isolation, and the shift/give-up arithmetic.

## Problem

`PortalUpdater::loop()` is **connect-once**: the first `sd_bus_process() < 0` (`pump_pending()`
returns false) logs `updater: sd_bus_process error — self-update inactive until next launch` and
**returns**, killing the watcher for the rest of the session. Two real triggers seen in the field
(durable/self-update.md):

1. **Our connection to the session `dbus-daemon` drops** (bus hiccup, or the app being torn down).
   Surfaces as `sd_bus_process() < 0`. The watcher goes inert; only a relaunch re-checks.
2. **flatpak-portal itself restarts** (e.g. a `--poll-timeout` drop-in change, a portal crash). Our
   connection to `dbus-daemon` stays alive, so `sd_bus_process()` does **not** error — but the
   `UpdateMonitor` object path we created is owned by the *old* portal process and is now dead, so we
   silently stop receiving `UpdateAvailable`/`Progress`. This is exactly the "portal restart orphans
   an already-created monitor" gotcha hit during the 2026-07-15 on-Deck verification: the app
   subscribes once and never recreates the monitor.

The original design (durable/self-update.md "Design decisions") deliberately chose connect-once with
recovery-by-relaunch. That is fine for case 1 at *teardown* (nothing left to watch), but wrong for a
mid-session drop or a portal restart during a long session: the user keeps the app open, an update
publishes, and it is never noticed until they happen to fully quit and relaunch.

## Goal

Make the watcher survive both failure modes **within the session**, without regressing any existing
invariant:

- **sd-bus stays single-threaded** — all bus work on the one owned loop thread; `request_update()`
  still only sets an atomic + nudges.
- **no reentrant `sd_bus_call`** inside `sd_bus_process()` — recovery is driven from the loop body,
  never from a signal callback (same rule `Update()` already follows).
- **idle costs no battery** — no new periodic wakeups; waits are `poll()`-bounded and interruptible.
- **`stop()` always joins promptly** — every wait watches the wake pipe, so a stop nudge breaks it.
- **a clean `stop()` is never mistaken for a failure** — recovery only runs while `running_` is true.
- **recovery-by-relaunch is preserved as the floor** — a *permanent* bus loss still eventually goes
  inert (bounded retries), so a wedged bus can't spin forever; relaunch remains the ultimate heal.

## Mechanism

Two independent recovery paths, sharing one `establish` refactor:

### A. Reconnect on bus error (case 1)
When `pump_pending()` returns false while `running_`, the loop tears the bus down and re-establishes
the whole connection (open bus → seed consent → create monitor → subscribe signals) with **bounded
exponential backoff**. On success it resumes `loop()`; if it exhausts the retry budget it logs and
goes inert (the pre-existing behaviour, just deferred by ~5 min of retries instead of immediate).

### B. Re-create the monitor on portal restart (case 2)
Subscribe once (per connection) to `org.freedesktop.DBus` **`NameOwnerChanged`** filtered to
`arg0=org.freedesktop.portal.Flatpak`. When the portal acquires a *new* owner (non-empty `arg2`), the
handler sets a `reestablish_monitor_` flag; the **loop** (not the callback — no reentrancy) unrefs the
stale monitor slots and calls `create_monitor()` + re-subscribes `UpdateAvailable`/`Progress` against
the new portal. The `dbus-daemon` connection and the `NameOwnerChanged` match are untouched — only the
monitor is rebuilt. The fresh monitor re-runs the portal's detection, so a pending update surfaces.

### Refactor
- `open_bus()` — `sd_bus_open_user`; sets `bus_`. Constructor still calls it once for `backend_live()`.
- `establish_monitor()` — unref `slot_avail_`/`slot_progress_`, `create_monitor(&monitor_path_)`,
  `sd_bus_match_signal` × 2; reset `updating_`/`want_update_` (an in-flight deploy is lost across a
  rebuild; the fresh monitor re-detects). Used by initial bring-up **and** case B.
- `establish()` — `open_bus()` if needed → `seed_update_permission()` →
  `subscribe_portal_owner_changes()` (case B match) → `establish_monitor()`. Rolls back (`teardown()`)
  on partial failure so a retry starts clean. Used by initial bring-up **and** case A.
- `teardown()` — unref all slots + the bus, null them, clear `monitor_path_`.
- `establish_with_retry()` — call `establish()`; on failure back off (`reconnect_delay_ms`) and retry
  until success, `stop()`, or `reconnect_should_give_up()`. `run()` and case A both go through it, so
  even the *first* monitor bring-up is now resilient (a portal not-yet-up at launch retries instead of
  going inert immediately).

`run()` becomes: `establish_with_retry()` → `loop()`. `loop()` gains two guarded branches: on
`!pump_pending()` do case A; when `reestablish_monitor_` is set do case B; otherwise the existing
`confirm_requested_`/`want_update_`/`do_update()` + `poll(bus_fd, wake_pipe, timeout)`.

## Pure, testable seams (compiled always, outside `DECKBACK_HAVE_SDBUS`, like `decode_progress_status`)

```cpp
std::uint64_t reconnect_delay_ms(unsigned attempt);        // 1000<<attempt, capped 60000
bool          reconnect_should_give_up(unsigned failures); // failures >= kReconnectMaxTries (12)
```

- `reconnect_delay_ms`: 0→1000, 1→2000, 2→4000, 3→8000, 4→16000, 5→32000, ≥6→60000 (shift-overflow
  guarded). L0-asserted in `updater_test.cpp`.
- `reconnect_should_give_up`: false for 0…11, true at ≥12 (≈1+2+4+8+16+32+60·6 ≈ 6 min of retries
  before inert). L0-asserted at the boundary.

The sd-bus event-loop recovery itself is integration-level (needs a real portal), so it is verified
**on-Deck**, not at L0 — drive it with `just portal-poll set 60` then restart the portal under a live
app and watch `journalctl --user` for the reconnect / monitor-rebuild lines. Same tier as the rest of
the portal path (durable/self-update.md).

## Out of scope / accepted limits

- No health-check *timer* polling the monitor's liveness — that would add idle wakeups the design
  forbids. Case B (owner-change) covers the only silent-orphan trigger we've actually observed.
- A **permanently** dead session bus still ends inert after the retry budget; relaunch heals it. This
  keeps the original recovery-by-relaunch floor; reconnect only removes the *transient*-drop and
  *portal-restart* cliffs.
- `selftest_deploy` keeps its own single-attempt connection (a diagnostic wants one clean cycle, not
  silent retries) — unchanged.
