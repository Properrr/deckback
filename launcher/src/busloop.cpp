#include "busloop.hpp"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <climits>

#if __has_include(<systemd/sd-bus.h>)
#include <time.h>
#endif

namespace deckback {
namespace {

constexpr std::uint64_t kReconnectBaseMs = 1000;
constexpr std::uint64_t kReconnectMaxMs = 60'000;
constexpr unsigned kReconnectMaxShift = 6;  // 1000<<6 = 64000 -> capped to kReconnectMaxMs
constexpr unsigned kReconnectMaxTries = 12;

int clamp_ms(std::uint64_t ms) {
  return ms > static_cast<std::uint64_t>(INT_MAX) ? INT_MAX : static_cast<int>(ms);
}

}  // namespace

std::uint64_t reconnect_delay_ms(unsigned attempt) {
  const unsigned shift = attempt < kReconnectMaxShift ? attempt : kReconnectMaxShift;
  const std::uint64_t d = kReconnectBaseMs << shift;
  return d < kReconnectMaxMs ? d : kReconnectMaxMs;
}

bool reconnect_should_give_up(unsigned consecutive_failures) {
  return consecutive_failures >= kReconnectMaxTries;
}

WakePipe::WakePipe() {
  if (pipe2(fds_, O_CLOEXEC | O_NONBLOCK) != 0) fds_[0] = fds_[1] = -1;
}

WakePipe::~WakePipe() {
  if (fds_[0] >= 0) close(fds_[0]);
  if (fds_[1] >= 0) close(fds_[1]);
}

void WakePipe::nudge() {
  if (fds_[1] < 0) return;
  const char b = 1;
  const ssize_t n = write(fds_[1], &b, 1);
  (void)n;  // a full pipe already holds a pending wake
}

void WakePipe::drain() {
  char buf[64];
  while (fds_[0] >= 0 && read(fds_[0], buf, sizeof buf) > 0) {
  }
}

bool WakePipe::wait(std::uint64_t ms) {
  pollfd pf{fds_[0], POLLIN, 0};
  poll(&pf, 1, clamp_ms(ms));
  if (!(pf.revents & POLLIN)) return false;
  drain();
  return true;
}

#if __has_include(<systemd/sd-bus.h>)

bool bus_pump(sd_bus* bus) {
  for (;;) {
    const int r = sd_bus_process(bus, nullptr);
    if (r < 0) return false;
    if (r == 0) return true;
  }
}

void bus_wait(sd_bus* bus, WakePipe& wake) {
  int timeout = -1;  // no pending sd-bus timeout: sleep until the bus or the pipe has something
  std::uint64_t until = 0;
  if (sd_bus_get_timeout(bus, &until) >= 0 && until != UINT64_MAX) {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const std::uint64_t now = static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000u +
                              static_cast<std::uint64_t>(ts.tv_nsec) / 1000u;
    timeout = until <= now ? 0 : clamp_ms((until - now + 999) / 1000);
  }
  pollfd fds[2] = {{sd_bus_get_fd(bus), static_cast<short>(sd_bus_get_events(bus)), 0},
                   {wake.read_fd(), POLLIN, 0}};
  poll(fds, 2, timeout);
  if (fds[1].revents & POLLIN) wake.drain();
}

#endif

}  // namespace deckback
