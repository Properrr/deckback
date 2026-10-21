#pragma once
#include <cstdint>

// The parts of an sd-bus event loop the updater (session bus, flatpak-portal) and the platform
// backend (system bus, logind) share: the reconnect backoff policy, the wake pipe other threads
// use to nudge the loop thread, and the pump/wait primitives. Both loops had their own copies, and
// only the updater's could reconnect; the platform loop warned once a second forever after a bus
// drop, with power integration dead (durable/dbus-reconnect.md).

namespace deckback {

// ---- reconnect backoff (pure, always compiled, L0-tested in updater_test.cpp)
// --------------------

// Exponential backoff from 1s, doubling, capped at 60s; the shift is clamped so any attempt index
// is defined (0→1000, 1→2000, … 5→32000, ≥6→60000).
std::uint64_t reconnect_delay_ms(unsigned attempt);
// After this many consecutive failed (re)connect attempts (~6–7 min of backoff) a loop goes inert
// until the next launch — the connect-once floor, just deferred.
bool reconnect_should_give_up(unsigned consecutive_failures);
// A reconnected link must stay up at least this long before it earns back a fresh retry budget, so
// a peer that connects-then-drops marches toward give-up instead of a tight reconnect loop.
inline constexpr long kReconnectStableMs = 30'000;

// ---- wake pipe ----------------------------------------------------------------------------------

// A self-pipe: any thread may nudge(); the loop thread polls read_fd() alongside the bus fd and
// drain()s it. Close-on-exec, because the engine is forked from this process.
class WakePipe {
 public:
  WakePipe();
  ~WakePipe();
  WakePipe(const WakePipe&) = delete;
  WakePipe& operator=(const WakePipe&) = delete;

  bool ok() const { return fds_[0] >= 0; }
  int read_fd() const { return fds_[0]; }
  void nudge();
  void drain();
  // Interruptible sleep: up to `ms`, ending early (and draining) on a nudge. True if nudged.
  bool wait(std::uint64_t ms);

 private:
  int fds_[2] = {-1, -1};
};

}  // namespace deckback

#if __has_include(<systemd/sd-bus.h>)
#include <systemd/sd-bus.h>

namespace deckback {

// Run every pending sd-bus dispatch. False when the connection has failed.
bool bus_pump(sd_bus* bus);

// Block until the bus has work, its next timeout is due, or the pipe is nudged (drained here).
// Never wakes on a timer of its own: an idle loop costs no battery.
void bus_wait(sd_bus* bus, WakePipe& wake);

}  // namespace deckback
#endif
