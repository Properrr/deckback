#pragma once
#include <string>

namespace deckback {

// Dependency-free network-reachability probes for the Phase 6 resume path (doc §2 P6): after wake
// we want the network back before nudging the player, so we don't drive it into an error state.

// One non-blocking TCP connect attempt to host:port, bounded by timeout_ms. True if it connects.
bool tcp_reachable(const std::string& host, int port, int timeout_ms);

// Poll tcp_reachable with backoff until it succeeds or max_ms elapses. max_ms <= 0 disables the
// wait (returns true immediately). Returns whether the host became reachable within the budget.
bool wait_online(const std::string& host, int port, int max_ms);

}  // namespace deckback
