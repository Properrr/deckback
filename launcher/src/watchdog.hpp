#pragma once
#include <string>
#include <vector>

namespace deckback {

struct Config;

// Spawns the Cobalt binary with flags from config and restarts it on crash (Leanback expects
// TV-style app-restart semantics, doc §2 P2). Rate-limited to avoid crash loops. Blocks until a
// clean exit or a shutdown signal.
class Watchdog {
 public:
  Watchdog(std::string cobalt_bin, std::vector<std::string> args, const Config& cfg);

  // Runs the supervise loop. Returns the child's final exit code (0 on clean shutdown).
  int run();

  // Async-signal-safe request to stop supervising and terminate the child.
  static void request_shutdown();

 private:
  std::string cobalt_bin_;
  std::vector<std::string> args_;
  bool restart_on_crash_;
  int max_restarts_per_minute_;
};

}  // namespace deckback
