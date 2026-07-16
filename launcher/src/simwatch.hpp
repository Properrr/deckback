#pragma once

// Launcher-side entry point for the containerized test SIMULATOR only — kept out of updater.cpp so
// the simulator surface is isolated from the shipping self-update logic (findings/durable/
// test-sim.md, durable/dbus-reconnect.md).
//
// This is the `--selftest-watch <secs>` mode. It drives the REAL updater loop (Updater::start())
// for a bounded window and does nothing else, so an external actor — the `just sim reconnect`
// harness — can drop the session bus (case A) or restart flatpak-portal (case B) and watch the
// reconnect handling log its progress. It uses only the public Updater interface; nothing here is
// compiled into the normal launch path beyond the one dispatch line in main().

namespace deckback {

// Run the updater loop for up to `secs` seconds (>=1), then stop cleanly. Notify mode with no
// consent, so nothing is ever deployed — the loop only watches. Interruptible by SIGINT/SIGTERM for
// a clean early stop (the harness signals once it has seen the reconnect it was testing, which also
// exercises stop()/join). Exit: 0 (ran and stopped) · 1 (no live session bus / stub build).
int run_selftest_watch(int secs);

}  // namespace deckback
