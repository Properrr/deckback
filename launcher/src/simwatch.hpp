#pragma once

// `--selftest-watch <secs>`, for the test simulator only: runs the real updater loop for a bounded
// window so the `just sim reconnect` harness can drop the session bus or restart flatpak-portal and
// watch the reconnect handling.

namespace deckback {

// Run the updater loop for up to `secs` seconds (>=1), then stop. Notify mode with no consent, so
// nothing is deployed. SIGINT/SIGTERM stop it early. Exit: 0 ran and stopped · 1 no live session
// bus (or stub build).
int run_selftest_watch(int secs);

}  // namespace deckback
