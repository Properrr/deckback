#include "simwatch.hpp"

#include <signal.h>
#include <time.h>

#include <chrono>
#include <csignal>
#include <memory>
#include <string>

#include "log.hpp"
#include "updater.hpp"

namespace deckback {

namespace {

// Set by SIGINT/SIGTERM so the bounded watch loop stops promptly and cleanly (the sim harness ends
// the drive with a signal once it has observed the reconnect it was testing).
volatile std::sig_atomic_t g_watch_stop = 0;
void watch_on_signal(int) { g_watch_stop = 1; }

}  // namespace

int run_selftest_watch(int secs) {
  info(std::string("selftest-watch: backend ") +
       (Updater::backend_available() ? "libsystemd" : "stub (no libsystemd)"));
  if (secs <= 0) secs = 1;

  // Default config = notify mode with no consent: the loop watches but never deploys. The point is
  // to keep the reconnect handling alive for the sim's case-A/case-B drive (durable/test-sim.md).
  auto up = Updater::create(UpdaterConfig{});
  if (!up->backend_live()) {
    error("selftest-watch: no live session bus (needs the Flatpak portal on the session bus)");
    return 1;
  }

  info("selftest-watch: running the updater loop for up to " + std::to_string(secs) +
       "s (portal reconnect drive; Ctrl-C / SIGTERM to stop early)");
  g_watch_stop = 0;
  struct sigaction sa {};
  sa.sa_handler = &watch_on_signal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  up->start();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(secs);
  while (std::chrono::steady_clock::now() < deadline && g_watch_stop == 0) {
    struct timespec ts {
      0, 200L * 1000L * 1000L
    };
    nanosleep(&ts, nullptr);  // EINTR on a signal → the g_watch_stop check ends the loop at once
  }
  info(std::string("selftest-watch: stopping (") + (g_watch_stop ? "signalled" : "window elapsed") +
       ")");
  up->stop();
  info("selftest-watch: done");
  return 0;
}

}  // namespace deckback
