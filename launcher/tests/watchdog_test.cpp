#include "watchdog.hpp"

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
  g_dir = fs::temp_directory_path() / "deckback_watchdog_test";
  fs::remove_all(g_dir);
  fs::create_directories(g_dir);

  test_clean_exit();
  test_crash_no_restart();
  test_crash_loop_gives_up();
  test_shutdown_is_success();  // must be last

  fs::remove_all(g_dir);
  std::puts("watchdog_test: ok");
  return 0;
}
