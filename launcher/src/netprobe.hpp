#pragma once
#include <functional>
#include <string>

namespace deckback {

// Dependency-free network-reachability probes for the Phase 6 resume path (doc §2 P6): after wake
// we want the network back before nudging the player, so we don't drive it into an error state.

// Connect to host:port, trying each resolved address with a non-blocking connect bounded by
// timeout_ms. Returns a connected, BLOCKING, close-on-exec fd the caller owns, or -1. The CDP
// client and the resume probe both use it, so they agree on what "reachable" means.
int connect_tcp(const std::string& host, int port, int timeout_ms);

// One connect_tcp attempt, closed again at once. True if it connected.
bool tcp_reachable(const std::string& host, int port, int timeout_ms);

// Poll tcp_reachable with backoff until it succeeds or max_ms elapses. max_ms <= 0 disables the
// wait (returns true immediately). Returns whether the host became reachable within the budget.
// `nap(ms)` sleeps between attempts and returns true to give up early (a shutdown, a new suspend);
// empty = a plain sleep.
bool wait_online(const std::string& host, int port, int max_ms,
                 const std::function<bool(int)>& nap = {});

}  // namespace deckback
