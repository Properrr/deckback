#include "watchdog.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "config.hpp"
#include "harness.hpp"

namespace fs = std::filesystem;
using namespace deckback;

namespace {

fs::path g_dir;

// Write an executable shell script and return its path.
std::string make_script(const std::string& name, const std::string& body) {
  fs::path p = g_dir / name;
  std::ofstream f(p);
  f << "#!/bin/sh\n" << body << "\n";
  f.close();
  fs::permissions(p, fs::perms::owner_all);
  return p.string();
}

Config cfg(bool restart, int max_per_min) {
  Config c;
  c.watchdog_restart_on_crash = restart;
  c.watchdog_max_restarts_per_minute = max_per_min;
  return c;
}

// A child that exits 0 is a clean, terminal success.
void test_clean_exit() {
  std::string bin = make_script("ok.sh", "exit 0");
  Watchdog wd(bin, {}, cfg(false, 5));
  assert(wd.run() == 0);
}

// A crash with restart disabled stops and reports failure.
void test_crash_no_restart() {
  std::string bin = make_script("bad.sh", "exit 3");
  Watchdog wd(bin, {}, cfg(false, 5));
  assert(wd.run() == 1);
}

// A tight crash loop is rate-limited and eventually gives up with a non-zero code.
void test_crash_loop_gives_up() {
  std::string bin = make_script("bad.sh", "exit 1");
  Watchdog wd(bin, {}, cfg(true, 3));
  assert(wd.run() == 1);
}

// The engine dies with a SIGKILLed launcher. This process becomes a subreaper, so the orphaned
// engine is re-parented here and its exit is observable with waitpid().
void test_engine_dies_with_a_killed_launcher() {
  const fs::path pid_file = g_dir / "engine.pid";
  std::string bin = make_script("engine.sh", "echo $$ > " + pid_file.string() + "; exec sleep 30");
  prctl(PR_SET_CHILD_SUBREAPER, 1);
  const pid_t launcher = fork();
  assert(launcher >= 0);
  if (launcher == 0) {
    Watchdog wd(bin, {}, cfg(false, 5));
    _exit(wd.run());
  }
  pid_t engine = -1;
  for (int i = 0; i < 200 && engine <= 0; ++i) {  // up to 2 s for the engine to start
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::ifstream f(pid_file);
    f >> engine;
  }
  assert(engine > 0);
  kill(launcher, SIGKILL);
  waitpid(launcher, nullptr, 0);

  bool reaped = false;
  for (int i = 0; i < 200 && !reaped; ++i) {  // up to 2 s; without PDEATHSIG it lives 30 s
    reaped = waitpid(engine, nullptr, WNOHANG) == engine;
    if (!reaped) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!reaped) kill(engine, SIGKILL);
  assert(reaped);
}

// The engine inherits only stdio, even an fd opened without O_CLOEXEC (like the log file).
void test_engine_inherits_only_stdio() {
  const fs::path probe = g_dir / "probe.txt";
  std::ofstream(probe) << "x";
  // Parked high: `ls` opens /proc/self/fd itself and gets the lowest free number, which would read
  // as a leak of a low probe fd that was in fact closed.
  const int low = ::open(probe.c_str(), O_RDONLY);
  const int leaked = fcntl(low, F_DUPFD, 40);
  ::close(low);
  assert(leaked >= 40);
  const fs::path listing = g_dir / "fds.txt";
  std::string bin = make_script("fds.sh", "ls /proc/self/fd > " + listing.string());
  Watchdog wd(bin, {}, cfg(false, 5));
  assert(wd.run() == 0);
  ::close(leaked);
  std::ifstream f(listing);
  for (std::string fd; f >> fd;) assert(fd != std::to_string(leaked));
}

// A requested shutdown (SIGTERM from Steam "Close game") is success, not a crash — even though the
// child dies by signal. Runs last: request_shutdown latches a process-global flag.
void test_shutdown_is_success() {
  std::string bin = make_script("sleep.sh", "exec sleep 30");
  Watchdog wd(bin, {}, cfg(false, 5));
  std::thread killer([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Watchdog::request_shutdown();
  });
  int rc = wd.run();
  killer.join();
  assert(rc == 0);
}

}  // namespace

DECKBACK_TEST_MAIN(watchdog) {
  g_dir = deckback_test_tmpdir() / "deckback_watchdog_test";
  fs::remove_all(g_dir);
  fs::create_directories(g_dir);

  test_clean_exit();
  test_crash_no_restart();
  test_crash_loop_gives_up();
  test_engine_dies_with_a_killed_launcher();
  test_engine_inherits_only_stdio();
  test_shutdown_is_success();  // must be last

  fs::remove_all(g_dir);
  std::puts("watchdog_test: ok");
  return 0;
}
