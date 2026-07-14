// L0 smoke for the self-updater lifecycle. The portal path itself (sd-bus → the Flatpak update
// portal) is integration-level and is verified on a Deck — `--selftest-update` plus a real update
// round-trip against a repo (findings/durable/self-update.md). Here we pin only the contract that
// holds off-Deck: the factory always yields a usable object, and the lifecycle is crash-free and
// idempotent regardless of whether a session bus / portal exists. Run under ASan (the test targets
// force -UNDEBUG) it also guards the thread-join lifetime — the exact bug an `exchange`-guarded
// stop() would have introduced (a self-exited thread destroyed while still joinable).
#include "updater.hpp"

#include <cassert>
#include <cstdio>

using namespace deckback;

int main() {
  // create() always yields a usable object: the real backend where sd-bus is compiled in, else a
  // stub.
  auto up = Updater::create(UpdaterConfig{});
  assert(up);
  (void)up->backend_live();  // must not crash whether or not a session bus exists

  // stop() before start() is a no-op.
  up->stop();

  // start()+stop() must be safe even when the portal is unreachable: start() may launch a thread
  // that self-exits on a failed CreateUpdateMonitor, and stop() must still join it — and be
  // idempotent.
  up->start();
  up->stop();
  up->stop();

  // A configured-but-disabled instance with a CDP port set is still a no-op: no toast can fire
  // without a delivered update, and a disabled updater never applies one.
  auto up2 = Updater::create(UpdaterConfig{.enabled = false, .cdp_port = 65000});
  up2->start();
  up2->stop();

  std::puts("updater: lifecycle ok");
  return 0;
}
