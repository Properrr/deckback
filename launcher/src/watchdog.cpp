#include "watchdog.hpp"

#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <format>
#include <vector>

#include "config.hpp"
#include "log.hpp"

namespace deckback {
namespace {

std::atomic<bool> g_shutdown{false};
std::atomic<pid_t> g_child{-1};

// Monotonic seconds, immune to wall-clock jumps.
long mono_seconds() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

}  // namespace

Watchdog::Watchdog(std::string cobalt_bin, std::vector<std::string> args, const Config& cfg)
    : cobalt_bin_(std::move(cobalt_bin)),
      args_(std::move(args)),
      restart_on_crash_(cfg.watchdog_restart_on_crash),
      max_restarts_per_minute_(cfg.watchdog_max_restarts_per_minute) {}

void Watchdog::request_shutdown() {
  g_shutdown.store(true);
  pid_t c = g_child.load();
  if (c > 0) kill(c, SIGTERM);
}

int Watchdog::run() {
  std::vector<char*> argv;
  argv.push_back(cobalt_bin_.data());
  for (auto& a : args_) argv.push_back(a.data());
  argv.push_back(nullptr);

  long window_start = mono_seconds();
  int restarts_in_window = 0;

  const pid_t launcher = getpid();
  while (!g_shutdown.load()) {
    pid_t pid = fork();
    if (pid < 0) {
      error(std::format("watchdog: fork failed: {}", std::strerror(errno)));
      return 1;
    }
    if (pid == 0) {
      // Die with the launcher: a killed launcher runs no shutdown path, and an orphaned engine
      // would keep the debugging port and profile in use. getppid() covers a launcher that died
      // before prctl().
      prctl(PR_SET_PDEATHSIG, SIGTERM);
      if (getppid() != launcher) _exit(0);
      // The engine inherits only stdio; this also covers fds opened without O_CLOEXEC.
      close_range(3, ~0U, CLOSE_RANGE_CLOEXEC);
      execv(cobalt_bin_.c_str(), argv.data());
      // Only reached if execv fails. write(), not perror(): after fork() in a threaded process, a
      // stdio lock held by another thread at fork time would deadlock the child.
      constexpr char kMsg[] = "deckback: execv of the engine failed\n";
      [[maybe_unused]] const ssize_t n = write(STDERR_FILENO, kMsg, sizeof kMsg - 1);
      _exit(127);
    }

    g_child.store(pid);
    // A shutdown request that landed between fork() and the store above saw no child to signal.
    if (g_shutdown.load()) kill(pid, SIGTERM);
    info(std::format("watchdog: started cobalt pid={}", pid));

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { /* retry across signals */
    }
    g_child.store(-1);

    // A requested shutdown (SIGTERM from us / Steam "Close game") is a success even though the
    // child dies by signal, so it must not be reported as a crash or a non-zero exit.
    if (g_shutdown.load()) {
      info("watchdog: shutdown requested; cobalt stopped");
      return 0;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
      info("watchdog: cobalt exited cleanly");
      return 0;
    }

    if (!restart_on_crash_) {
      warn("watchdog: cobalt crashed; restart disabled by config");
      return 1;
    }

    long now = mono_seconds();
    if (now - window_start >= 60) {
      window_start = now;
      restarts_in_window = 0;
    }
    if (++restarts_in_window > max_restarts_per_minute_) {
      error(std::format("watchdog: crash loop ({} restarts/min) — giving up", restarts_in_window));
      return 1;
    }
    warn(std::format("watchdog: cobalt crashed (status={}); restart {}/{}", status,
                     restarts_in_window, max_restarts_per_minute_));
  }
  return 0;
}

}  // namespace deckback
