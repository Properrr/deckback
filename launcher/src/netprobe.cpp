#include "netprobe.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>

namespace deckback {
namespace {

long mono_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1'000'000L;
}

}  // namespace

bool tcp_reachable(const std::string& host, int port, int timeout_ms) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string port_s = std::to_string(port);
  if (getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0 || !res) return false;

  bool ok = false;
  for (addrinfo* ai = res; ai && !ok; ai = ai->ai_next) {
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc == 0) {
      ok = true;
    } else if (errno == EINPROGRESS) {
      pollfd pfd{fd, POLLOUT, 0};
      if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT)) {
        int err = 0;
        socklen_t l = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
        ok = (err == 0);
      }
    }
    close(fd);
  }
  freeaddrinfo(res);
  return ok;
}

bool wait_online(const std::string& host, int port, int max_ms) {
  if (max_ms <= 0) return true;
  const long deadline = mono_ms() + max_ms;
  int backoff = 200;
  for (;;) {
    int remaining = static_cast<int>(deadline - mono_ms());
    if (remaining <= 0) return false;
    if (tcp_reachable(host, port, remaining < 1000 ? remaining : 1000)) return true;
    int nap = backoff < remaining ? backoff : remaining;
    if (nap > 0) {
      timespec ts{nap / 1000, (nap % 1000) * 1'000'000L};
      nanosleep(&ts, nullptr);
    }
    backoff = backoff < 1600 ? backoff * 2 : 1600;
  }
}

}  // namespace deckback
