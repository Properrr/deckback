#include "updater.hpp"

#include "log.hpp"

// sd-bus wiring is gated on the header being present so this file compiles on a bare toolchain (the
// CI free-runner has no libsystemd), exactly like platform.cpp.
#if __has_include(<systemd/sd-bus.h>)
#define DECKBACK_HAVE_SDBUS 1
#include <fcntl.h>
#include <poll.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>

#include "devtools.hpp"
#include "overlay.hpp"
#else
#define DECKBACK_HAVE_SDBUS 0
#endif

namespace deckback {
namespace {

class StubUpdater final : public Updater {
 public:
  void start() override {}
  void stop() override {}
  bool backend_live() const override { return false; }
  int selftest() override {
    error("selftest-update: no libsystemd backend compiled in");
    return 1;
  }
};

#if DECKBACK_HAVE_SDBUS

constexpr const char* kPortal = "org.freedesktop.portal.Flatpak";
constexpr const char* kPortalPath = "/org/freedesktop/portal/Flatpak";
constexpr const char* kPortalIface = "org.freedesktop.portal.Flatpak";
constexpr const char* kMonitorIface = "org.freedesktop.portal.Flatpak.UpdateMonitor";

// Progress.status values (org.freedesktop.portal.Flatpak.xml): 0 Running, 1 Empty (nothing to do),
// 2 Done, 3 Failed.
enum : uint32_t { kRunning = 0, kEmpty = 1, kDone = 2, kFailed = 3 };

// Bound the two synchronous portal calls so a missing/slow portal fails fast on our background
// thread instead of hanging on sd-bus's 25 s default (and so the L0 lifecycle test cannot stall).
// These are local D-Bus calls that return an ack, not the deploy itself — progress arrives via
// signals.
constexpr uint64_t kCallTimeoutUsec = 5'000'000;

// Scan one `a{sv}` for at most one `u`-typed key and one `s`-typed key, ignoring everything else.
// The portal's Progress dict is {n_ops:u, op:u, progress:u, status:u, error:s, error_message:s};
// UpdateAvailable is {running-commit:s, local-commit:s, remote-commit:s}. Either key may be null.
void scan_dict(sd_bus_message* m, const char* want_u, uint32_t* out_u, const char* want_s,
               std::string* out_s) {
  if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return;
  while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
    const char* key = nullptr;
    if (sd_bus_message_read(m, "s", &key) < 0 || !key) {
      sd_bus_message_exit_container(m);
      break;
    }
    if (want_u && std::strcmp(key, want_u) == 0) {
      if (sd_bus_message_enter_container(m, 'v', "u") > 0) {
        sd_bus_message_read(m, "u", out_u);
        sd_bus_message_exit_container(m);
      } else {
        sd_bus_message_skip(m, "v");
      }
    } else if (want_s && std::strcmp(key, want_s) == 0) {
      const char* s = nullptr;
      if (sd_bus_message_enter_container(m, 'v', "s") > 0) {
        if (sd_bus_message_read(m, "s", &s) >= 0 && s) *out_s = s;
        sd_bus_message_exit_container(m);
      } else {
        sd_bus_message_skip(m, "v");
      }
    } else {
      sd_bus_message_skip(m, "v");
    }
    sd_bus_message_exit_container(m);  // exit the "sv" dict entry
  }
  sd_bus_message_exit_container(m);  // exit the array
}

// Session-bus client for the Flatpak update portal. All sd-bus calls happen on one owned event-loop
// thread (sd-bus is not thread-safe); the monitor + signal matches are set up on that thread too.
class PortalUpdater final : public Updater {
 public:
  explicit PortalUpdater(UpdaterConfig cfg) : cfg_(std::move(cfg)) {
    if (sd_bus_open_user(&bus_) < 0 || !bus_) {
      warn("updater: sd_bus_open_user failed — self-update disabled");
      return;
    }
    if (pipe(wake_pipe_) == 0) fcntl(wake_pipe_[0], F_SETFL, O_NONBLOCK);
    live_ = true;
  }

  ~PortalUpdater() override {
    stop();
    if (slot_avail_) sd_bus_slot_unref(slot_avail_);
    if (slot_progress_) sd_bus_slot_unref(slot_progress_);
    if (wake_pipe_[0] >= 0) close(wake_pipe_[0]);
    if (wake_pipe_[1] >= 0) close(wake_pipe_[1]);
    if (bus_) sd_bus_unref(bus_);
  }

  void start() override {
    if (!live_ || started_) return;  // not restartable; one monitor per process is all we need
    started_ = true;
    running_.store(true);
    thread_ = std::thread([this] { run(); });
  }

  // Idempotent: always joins the thread, even if run() self-exited early (a failed monitor setup),
  // so the std::thread is never destroyed while joinable.
  void stop() override {
    running_.store(false);
    nudge();
    if (thread_.joinable()) thread_.join();
  }

  bool backend_live() const override { return live_; }

  int selftest() override {
    if (!live_) {
      error("selftest-update: no live session bus");
      return 1;
    }
    std::string path;
    if (!create_monitor(&path)) {
      error(
          "selftest-update: CreateUpdateMonitor failed — is the Flatpak portal reachable? (run "
          "this "
          "inside the flatpak; the host needs flatpak >= 1.5)");
      return 1;
    }
    info("selftest-update: portal answered; update monitor at " + path);
    return 0;
  }

 private:
  bool create_monitor(std::string* out_path) {
    sd_bus_message* req = nullptr;
    if (sd_bus_message_new_method_call(bus_, &req, kPortal, kPortalPath, kPortalIface,
                                       "CreateUpdateMonitor") < 0)
      return false;
    sd_bus_message_open_container(req, 'a', "{sv}");  // empty options
    sd_bus_message_close_container(req);
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call(bus_, req, kCallTimeoutUsec, &err, &reply);
    sd_bus_message_unref(req);
    if (r < 0) {
      warn(std::string("updater: CreateUpdateMonitor: ") + (err.message ? err.message : "error"));
      sd_bus_error_free(&err);
      return false;
    }
    const char* path = nullptr;
    r = sd_bus_message_read(reply, "o", &path);
    if (r >= 0 && path) *out_path = path;
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    return r >= 0 && !out_path->empty();
  }

  void run() {
    if (!create_monitor(&monitor_path_)) {
      error("updater: could not create the update monitor — self-update inactive this session");
      running_.store(false);
      return;
    }
    sd_bus_match_signal(bus_, &slot_avail_, kPortal, monitor_path_.c_str(), kMonitorIface,
                        "UpdateAvailable", &on_update_available, this);
    sd_bus_match_signal(bus_, &slot_progress_, kPortal, monitor_path_.c_str(), kMonitorIface,
                        "Progress", &on_progress, this);
    info("updater: watching for updates to this app via the Flatpak portal");
    loop();
  }

  void loop() {
    while (running_.load()) {
      for (;;) {
        int r = sd_bus_process(bus_, nullptr);
        if (r < 0) {
          warn("updater: sd_bus_process error");
          return;
        }
        if (r == 0) break;
      }
      if (!running_.load()) break;
      // Kick the deploy outside the signal callback, so we never call sd_bus_call() reentrantly
      // from inside sd_bus_process(). want_update_/updating_ are touched only on this thread.
      if (want_update_ && !updating_) {
        want_update_ = false;
        updating_ = true;
        do_update();
      }
      struct pollfd fds[2];
      fds[0].fd = sd_bus_get_fd(bus_);
      fds[0].events = static_cast<short>(sd_bus_get_events(bus_));
      fds[0].revents = 0;
      fds[1].fd = wake_pipe_[0];
      fds[1].events = POLLIN;
      fds[1].revents = 0;
      poll(fds, 2, loop_timeout_ms());
      if (fds[1].revents & POLLIN) drain_wake();
    }
  }

  void do_update() {
    sd_bus_message* req = nullptr;
    if (sd_bus_message_new_method_call(bus_, &req, kPortal, monitor_path_.c_str(), kMonitorIface,
                                       "Update") < 0) {
      updating_ = false;
      return;
    }
    sd_bus_message_append(req, "s", "");              // parent_window: none
    sd_bus_message_open_container(req, 'a', "{sv}");  // empty options
    sd_bus_message_close_container(req);
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call(bus_, req, kCallTimeoutUsec, &err, &reply);
    sd_bus_message_unref(req);
    if (r < 0) {
      warn(std::string("updater: Update(): ") + (err.message ? err.message : "error"));
      updating_ = false;
    } else {
      info("updater: update requested; deploying in the background");
      sd_bus_message_unref(reply);
    }
    sd_bus_error_free(&err);
  }

  void announce_ready() {
    if (cfg_.cdp_port <= 0) return;
    // A transient client: connect, toast, disconnect. The event is rare, so a persistent CDP
    // connection just for this is not worth its reconnect bookkeeping. Fire-and-forget — a toast
    // that fails to render must never affect the update it is announcing.
    DevToolsClient client(cfg_.cdp_host, cfg_.cdp_port);
    show_toast(client, cfg_.toast_text, cfg_.toast_ms);
  }

  static int on_update_available(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<PortalUpdater*>(userdata);
    std::string remote;
    scan_dict(m, nullptr, nullptr, "remote-commit", &remote);
    info("updater: an update is available" +
         (remote.empty() ? std::string() : " (remote " + remote.substr(0, 12) + ")"));
    if (self->cfg_.enabled && !self->updating_)
      self->want_update_ = true;  // deploy is kicked from the loop, not here
    else if (!self->cfg_.enabled)
      info("updater: self_update is off — not applying (update manually or enable self_update)");
    return 0;
  }

  static int on_progress(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<PortalUpdater*>(userdata);
    uint32_t status = kRunning;
    std::string errmsg;
    scan_dict(m, "status", &status, "error_message", &errmsg);
    switch (status) {
      case kDone:
        info("updater: update deployed — it will apply the next time Deckback is launched");
        self->updating_ = false;
        self->announce_ready();
        break;
      case kFailed:
        warn("updater: update failed: " + (errmsg.empty() ? "unknown error" : errmsg));
        self->updating_ = false;
        break;
      case kEmpty:
        self->updating_ = false;
        break;
      default:  // kRunning — in-flight, wait for the next Progress
        break;
    }
    return 0;
  }

  int loop_timeout_ms() {
    uint64_t until = 0;
    int r = sd_bus_get_timeout(bus_, &until);
    if (r < 0 || until == UINT64_MAX) return 1000;
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = static_cast<uint64_t>(ts.tv_sec) * 1'000'000ull + ts.tv_nsec / 1000;
    if (until <= now) return 0;
    uint64_t ms = (until - now + 999) / 1000;
    return ms > 1000 ? 1000 : static_cast<int>(ms);
  }

  void nudge() {
    if (wake_pipe_[1] >= 0) {
      const char b = 1;
      ssize_t n = write(wake_pipe_[1], &b, 1);
      (void)n;
    }
  }
  void drain_wake() {
    char buf[64];
    while (read(wake_pipe_[0], buf, sizeof buf) > 0) { /* drain */
    }
  }

  UpdaterConfig cfg_;
  sd_bus* bus_ = nullptr;
  sd_bus_slot* slot_avail_ = nullptr;
  sd_bus_slot* slot_progress_ = nullptr;
  std::string monitor_path_;
  int wake_pipe_[2] = {-1, -1};
  std::atomic<bool> running_{false};
  bool started_ = false;      // start()-thread only; guards one-shot launch
  bool want_update_ = false;  // loop-thread only
  bool updating_ = false;     // loop-thread only
  bool live_ = false;
  std::thread thread_;
};

#endif  // DECKBACK_HAVE_SDBUS

}  // namespace

bool Updater::backend_available() { return DECKBACK_HAVE_SDBUS != 0; }

std::unique_ptr<Updater> Updater::create(UpdaterConfig cfg) {
#if DECKBACK_HAVE_SDBUS
  return std::make_unique<PortalUpdater>(std::move(cfg));
#else
  (void)cfg;
  warn("updater: no libsystemd — self-update disabled (stub backend)");
  return std::make_unique<StubUpdater>();
#endif
}

}  // namespace deckback
