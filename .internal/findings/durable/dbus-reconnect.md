# D-Bus reconnect (self-updater and platform loops)

Both of the launcher's sd-bus loops survive a dropped bus within the session: the self-updater's
session-bus loop (`launcher/src/updater.cpp`, talks to flatpak-portal) and the platform loop's
system-bus connection (`launcher/src/platform.cpp`, logind). The shared policy, wake pipe and
pump/wait primitives live in `launcher/src/busloop.{hpp,cpp}`.

## Status

| Path | Verified by | Not verified |
|---|---|---|
| Backoff / give-up arithmetic | L0, `updater_test.cpp` | — |
| Updater case A (bus drop) + case B (portal restart) | `just sim reconnect`: the real `--selftest-watch` loop under a bwrap flatpak-instance (`test-sim.md`) | an on-Deck drive (`systemctl --user restart flatpak-portal` and a bus drop, watching `journalctl --user` for `flatpak-portal restarted — re-creating the update monitor` / `reconnected to the Flatpak portal`) |
| Platform loop reconnect | L1, `launcher/tests/platform_test.cpp`: kills and restarts a private `dbus-daemon` acting as the system bus under a live connection, asserts a second `PrepareForSleep` arrives | a real system-bus drop on a Deck — there is no safe way to cause one (restarting dbus or logind ends the Game Mode session); only "suspend still works" (`just soak`) is checkable there |

The `platform_test` daemon policy must spell out `allow user="*"`, `send_destination="*"`,
`receive_sender="*"` and `own="*"`: with any one missing, the daemon accepts the socket and drops the
client at Hello (sd-bus then reports `ENOTCONN` on the first call).

## Backoff policy (`busloop`)

```cpp
std::uint64_t reconnect_delay_ms(unsigned attempt);        // 1000 << min(attempt, 6), capped at 60000
bool          reconnect_should_give_up(unsigned failures); // failures >= 12
inline constexpr long kReconnectStableMs = 30'000;
```

- Delays: 0→1000, 1→2000, 2→4000, 3→8000, 4→16000, 5→32000, ≥6→60000 ms (shift clamped, so no
  overflow). Twelve failures ≈ 6 min of retries, then the loop goes inert.
- The retry budget resets **only after a connection stayed up ≥ 30 s** (monotonic clock). A
  connection that drops sooner counts as a failure, so a connect-then-drop flap backs off and
  eventually goes inert instead of spinning.
- Both are pure and compiled even without `<systemd/sd-bus.h>`, so L0 runs without libsystemd.

## Updater: two recovery paths

**A. Reconnect on bus error.** When `pump_pending()` (`sd_bus_process() < 0`) fails while running,
the loop tears the bus down and re-runs `establish()` (open bus → seed consent → subscribe to portal
owner changes → create monitor + subscribe `UpdateAvailable`/`Progress`) through
`establish_with_retry()`. On give-up it returns from `loop()` (the bus is null). The very first
bring-up goes through the same retry, so a portal not yet up at launch retries rather than going
inert.

**B. Re-create the monitor on portal restart.** If flatpak-portal restarts, our `dbus-daemon`
connection survives but the `UpdateMonitor` object path belongs to the dead portal process, so
signals silently stop. The loop subscribes once per connection with an explicit match string —
`type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.freedesktop.portal.Flatpak'`
(`sd_bus_add_match`; `sd_bus_match_signal` has no `arg0` filter and would wake on every name change).
Args are `(name, old_owner, new_owner)`; only a non-empty `new_owner` acts. The callback sets
`reestablish_monitor_`; the **loop** (not the callback) unrefs the stale monitor slots and recreates
the monitor. The fresh monitor re-runs the portal's detection, so a pending update surfaces.

## Invariants

- **sd-bus stays single-threaded** — all bus work on the loop thread; `request_update()` only sets
  an atomic and nudges the wake pipe.
- **No reentrant `sd_bus_call` inside `sd_bus_process()`** — recovery runs from the loop body, never
  from a signal callback.
- **Idle costs no battery** — no periodic wakeups or liveness timer; every wait is a `poll()` on the
  wake pipe and is interruptible.
- **`stop()` joins promptly.** The wake pipe cannot interrupt an in-flight `sd_bus_call` (each bounded
  by `kCallTimeoutUsec` = 5 s), so `establish()` checks `running_` before each blocking step,
  including between the two consent-seed calls. Worst-case join is one in-flight ~5 s call. Every
  retry iteration also checks `running_`.
- **A clean `stop()` is never treated as a failure**; `run()` sets `running_ = false` on every exit
  path.
- **Consent is seeded once per session** (`seeded_`), and latched only on a determinate outcome (not
  sandboxed, already yes/no, or a grant that succeeded) — a transient seed failure re-seeds on the
  next reconnect.
- **A monitor rebuild resets `updating_` but keeps `want_update_`**, so a latched "Update now" survives
  a portal restart.
- **Teardown unrefs slots before the bus** (slots hold a bus ref). `slot_owner_` is recreated in
  `establish()`.
- **`selftest_deploy` keeps its own single-attempt connection**, never routed through the retry path.

## Platform loop

Same policy. After a reconnect it re-subscribes `PrepareForSleep` and re-takes only an inhibitor lock
that is not held: the lock fds are logind FIFOs that outlive the bus connection.

## Accepted limits

- A **permanently** dead bus still ends inert after the retry budget; a relaunch heals it.
- No liveness timer polls the monitor; case B (owner change) covers the only silent-orphan trigger
  observed.
