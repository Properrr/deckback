// The logind backend's reconnect, against a real dbus-daemon standing in for the system bus. A
// private bus (DBUS_SYSTEM_BUS_ADDRESS) carries a PrepareForSleep to the suspend hook; then the
// daemon is restarted on the same socket, and a second PrepareForSleep must arrive too.
//
// SKIPs (passes) without libsystemd or dbus-daemon. logind is not on this bus, so the Inhibit calls
// fail and are logged; only the PrepareForSleep(true) path is driven, because the (false) path
// re-takes the delay lock with a call nobody here would answer.
#include "platform.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "harness.hpp"

#if __has_include(<systemd/sd-bus.h>)
#include <systemd/sd-bus.h>
#define DECKBACK_TEST_SDBUS 1
#else
#define DECKBACK_TEST_SDBUS 0
#endif

using namespace deckback;

namespace {

#if DECKBACK_TEST_SDBUS

namespace fs = std::filesystem;

struct PrivateBus {
  fs::path dir = deckback_test_tmpdir() / "bus";
  fs::path socket = dir / "system_bus_socket";
  fs::path config = dir / "bus.conf";
  pid_t pid = -1;

  std::string address() const { return "unix:path=" + socket.string(); }

  bool connectable() const {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::snprintf(sa.sun_path, sizeof sa.sun_path, "%s", socket.c_str());
    const bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0;
    ::close(fd);
    return ok;
  }

  bool start() {
    fs::create_directories(dir);
    std::ofstream(config) << "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus "
                             "Configuration 1.0//EN\"\n"
                             " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
                             "<busconfig><type>session</type>"
                             "<listen>unix:path="
                          << socket.string()
                          << "</listen><auth>EXTERNAL</auth>"
                             // Allow-all, spelled out: with any of these missing the daemon
                             // accepts the socket, then drops the client at Hello.
                             "<policy context=\"default\"><allow user=\"*\"/>"
                             "<allow send_destination=\"*\"/><allow receive_sender=\"*\"/>"
                             "<allow own=\"*\"/></policy></busconfig>\n";
    std::error_code ec;
    fs::remove(socket, ec);
    const std::string cfg = "--config-file=" + config.string();
    const pid_t parent = getpid();
    pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
      // Dies with the test (an assert() abort runs no destructor), and keeps off ctest's output
      // pipe so ctest never waits on it for EOF.
      prctl(PR_SET_PDEATHSIG, SIGKILL);
      if (getppid() != parent) _exit(1);
      const int null = open("/dev/null", O_RDWR | O_CLOEXEC);
      dup2(null, 0);
      dup2(null, 1);
      dup2(null, 2);
      execlp("dbus-daemon", "dbus-daemon", cfg.c_str(), "--nofork", "--nopidfile", nullptr);
      _exit(127);
    }
    for (int i = 0; i < 100 && !connectable(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return connectable();
  }

  void stop() {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
    pid = -1;
  }

  ~PrivateBus() { stop(); }
};

// A peer that owns org.freedesktop.login1 on the private bus and emits PrepareForSleep(true).
bool emit_prepare_for_sleep(const std::string& address) {
  sd_bus* bus = nullptr;
  bool ok = sd_bus_new(&bus) >= 0 && sd_bus_set_address(bus, address.c_str()) >= 0 &&
            sd_bus_set_bus_client(bus, 1) >= 0 && sd_bus_start(bus) >= 0 &&
            sd_bus_request_name(bus, "org.freedesktop.login1", 0) >= 0 &&
            sd_bus_emit_signal(bus, "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                               "PrepareForSleep", "b", 1) >= 0 &&
            sd_bus_flush(bus) >= 0;
  sd_bus_flush_close_unref(bus);
  return ok;
}

bool wait_for(const std::atomic<int>& n, int want, int ms, const std::string& address) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    emit_prepare_for_sleep(address);  // lost harmlessly until the platform has (re)subscribed
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (n.load() >= want) return true;
  }
  return n.load() >= want;
}

#endif

}  // namespace

DECKBACK_TEST_MAIN(platform) {
#if DECKBACK_TEST_SDBUS
  if (std::system("command -v dbus-daemon >/dev/null 2>&1") != 0) {
    std::puts("SKIP platform_test: dbus-daemon not installed");
    return 0;
  }
  PrivateBus bus;
  if (!bus.start()) {
    std::puts("SKIP platform_test: could not start a private dbus-daemon");
    return 0;
  }
  setenv("DBUS_SYSTEM_BUS_ADDRESS", bus.address().c_str(), 1);

  std::atomic<int> suspends{0};
  {
    std::unique_ptr<Platform> p = Platform::create();
    assert(p->backend_live());
    p->on_suspend([&] { suspends.fetch_add(1); });

    assert(wait_for(suspends, 1, 3000, bus.address()));

    bus.stop();           // the system bus goes away under the live connection...
    assert(bus.start());  // ...and comes back on the same socket
    const int before = suspends.load();
    assert(wait_for(suspends, before + 1, 10'000, bus.address()));
    assert(p->backend_live());
  }  // destruction must join promptly even mid-backoff
  std::puts("platform_test: reconnected and re-subscribed");
#else
  std::puts("SKIP platform_test: no libsystemd");
#endif
  return 0;
}
