#include "platform.hpp"

#include <cstdlib>

#include "log.hpp"

// sd-bus wiring is gated on the header being present so this file compiles on a bare toolchain.
#if __has_include(<systemd/sd-bus.h>)
#define DECKBACK_HAVE_SDBUS 1
#include <fcntl.h>
#include <poll.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <mutex>
#include <string_view>
#include <thread>
#else
#define DECKBACK_HAVE_SDBUS 0
#endif

namespace deckback {
namespace {

class StubPlatform final : public Platform {
 public:
  void on_suspend(std::function<void()>) override {}
  void on_resume(std::function<void()>) override {}
  void set_idle_inhibited(bool) override {}
  bool backend_live() const override { return false; }
  int selftest() override {
    error("selftest: no libsystemd backend compiled in");
    return 1;
  }
};

#if DECKBACK_HAVE_SDBUS

constexpr const char* kLogind = "org.freedesktop.login1";
constexpr const char* kLogindPath = "/org/freedesktop/login1";
constexpr const char* kManager = "org.freedesktop.login1.Manager";

constexpr const char* kSystemd = "org.freedesktop.systemd1";
constexpr const char* kSystemdPath = "/org/freedesktop/systemd1";
constexpr const char* kSystemdManager = "org.freedesktop.systemd1.Manager";
constexpr const char* kSystemdUnit = "org.freedesktop.systemd1.Unit";
// GetUnit's error for a unit that is not loaded — a real "not installed", not a failure to ask.
constexpr const char* kNoSuchUnit = "org.freedesktop.systemd1.NoSuchUnit";

// Implements the Phase 6 power contract against logind:
//   * a "sleep"/delay inhibitor is held so on_suspend runs before the system sleeps;
//   * an "idle"/block inhibitor is held while the player is playing so the screen never dims.
// All sd-bus calls happen on a single owned event-loop thread (sd-bus is not thread-safe); other
// threads request idle-inhibit changes via an atomic + self-pipe wakeup.
class SdBusPlatform final : public Platform {
 public:
  SdBusPlatform() {
    if (sd_bus_open_system(&bus_) < 0 || !bus_) {
      warn("platform: sd_bus_open_system failed — running without power integration");
      return;
    }
    if (pipe(wake_pipe_) == 0) {
      fcntl(wake_pipe_[0], F_SETFL, O_NONBLOCK);
    }
    sleep_fd_ = take_inhibitor("sleep", "Deckback: pause/checkpoint before suspend", "delay");
    sd_bus_match_signal(bus_, &slot_, kLogind, kLogindPath, kManager, "PrepareForSleep",
                        &on_prepare_for_sleep, this);
    live_ = true;
    running_.store(true);
    thread_ = std::thread([this] { loop(); });
    info("platform: logind backend live");
  }

  ~SdBusPlatform() override {
    running_.store(false);
    nudge();
    if (thread_.joinable()) thread_.join();
    if (idle_fd_ >= 0) close(idle_fd_);
    if (sleep_fd_ >= 0) close(sleep_fd_);
    if (wake_pipe_[0] >= 0) close(wake_pipe_[0]);
    if (wake_pipe_[1] >= 0) close(wake_pipe_[1]);
    if (slot_) sd_bus_slot_unref(slot_);
    if (bus_) sd_bus_unref(bus_);
  }

  void on_suspend(std::function<void()> cb) override {
    std::lock_guard lk(cb_mutex_);
    suspend_cb_ = std::move(cb);
  }
  void on_resume(std::function<void()> cb) override {
    std::lock_guard lk(cb_mutex_);
    resume_cb_ = std::move(cb);
  }

  void set_idle_inhibited(bool inhibited) override {
    want_idle_.store(inhibited);
    nudge();
  }

  bool backend_live() const override { return live_; }

  int selftest() override {
    if (!live_) {
      error("selftest: no live logind connection");
      return 1;
    }
    info(
        std::format("selftest: sleep(delay) inhibitor {}", sleep_fd_ >= 0 ? "acquired" : "FAILED"));
    set_idle_inhibited(true);
    // Give the event-loop thread a moment to reconcile the idle inhibitor, then list ours.
    struct timespec ts {
      0, 200'000'000
    };
    nanosleep(&ts, nullptr);
    info("selftest: current Deckback logind inhibitors —");
    int rc = std::system(
        "systemd-inhibit --list --no-pager 2>/dev/null | grep -i deckback || echo '  (none "
        "listed)'");
    (void)rc;
    set_idle_inhibited(false);
    nanosleep(&ts, nullptr);
    return sleep_fd_ >= 0 ? 0 : 1;
  }

 private:
  // Returns a dup'd, owned fd for the inhibitor lock, or -1 on failure. Closing it releases.
  int take_inhibitor(const char* what, const char* why, const char* mode) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call_method(bus_, kLogind, kLogindPath, kManager, "Inhibit", &err, &reply,
                               "ssss", what, "Deckback", why, mode);
    if (r < 0) {
      warn(std::format("platform: Inhibit({}) failed: {}", what,
                       err.message ? err.message : "unknown"));
      sd_bus_error_free(&err);
      return -1;
    }
    int fd = -1;
    r = sd_bus_message_read(reply, "h", &fd);
    int owned = (r >= 0 && fd >= 0) ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : -1;
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    return owned;
  }

  // Hold an "idle" block inhibitor while a video plays. On the Deck this is otherwise harmless
  // (logind's idle action does nothing under gamescope), but its "Deckback: playback active" reason
  // is the signal the host-side idle-nudge helper gates on (scripts/idle-nudge.py).
  //
  // We deliberately do NOT hold a "sleep" block. Preventing Steam Game Mode's auto-suspend is the
  // idle-nudge helper's job: it resets gamescope's input-idle timer with a *real* synthetic input,
  // which also keeps the screen on AND leaves a deliberate power-button sleep working. A
  // launcher-side "sleep" block only ever blocked suspend indiscriminately — including the power
  // button mid-playback — for no benefit the helper doesn't cover better. (History: a sleep-block
  // shipped briefly on 2026-07-11, then was retired once the on-Deck test showed gamescope ignores
  // sandbox-emulated input for idle, forcing the host helper anyway — see docs/SUPPORT.md.)
  void reconcile_idle() {
    const bool want = want_idle_.load();
    if (want && idle_fd_ < 0) {
      idle_fd_ = take_inhibitor("idle", "Deckback: playback active", "block");
      if (idle_fd_ >= 0) info("platform: idle inhibitor held (playing)");
    } else if (!want && idle_fd_ >= 0) {
      close(idle_fd_);
      idle_fd_ = -1;
      info("platform: idle inhibitor released");
    }
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

  static int on_prepare_for_sleep(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<SdBusPlatform*>(userdata);
    int going_to_sleep = 0;
    if (sd_bus_message_read(m, "b", &going_to_sleep) < 0) return 0;
    if (going_to_sleep) {
      info("platform: PrepareForSleep(true) — running suspend hook, then releasing delay lock");
      {
        std::lock_guard lk(self->cb_mutex_);
        if (self->suspend_cb_) self->suspend_cb_();
      }
      if (self->sleep_fd_ >= 0) {
        close(self->sleep_fd_);  // release the delay inhibitor -> system proceeds to sleep
        self->sleep_fd_ = -1;
      }
    } else {
      info("platform: PrepareForSleep(false) — resumed; re-arming delay lock, running resume hook");
      self->sleep_fd_ =
          self->take_inhibitor("sleep", "Deckback: pause/checkpoint before suspend", "delay");
      {
        std::lock_guard lk(self->cb_mutex_);
        if (self->resume_cb_) self->resume_cb_();
      }
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

  void loop() {
    while (running_.load()) {
      for (;;) {
        int r = sd_bus_process(bus_, nullptr);
        if (r < 0) {
          warn("platform: sd_bus_process error");
          break;
        }
        if (r == 0) break;
      }
      if (!running_.load()) break;

      struct pollfd fds[2];
      fds[0].fd = sd_bus_get_fd(bus_);
      fds[0].events = static_cast<short>(sd_bus_get_events(bus_));
      fds[0].revents = 0;
      fds[1].fd = wake_pipe_[0];
      fds[1].events = POLLIN;
      fds[1].revents = 0;

      poll(fds, 2, loop_timeout_ms());

      if (fds[1].revents & POLLIN) {
        drain_wake();
        reconcile_idle();
      }
    }
  }

  sd_bus* bus_ = nullptr;
  sd_bus_slot* slot_ = nullptr;
  int sleep_fd_ = -1;
  int idle_fd_ = -1;
  int wake_pipe_[2] = {-1, -1};
  std::atomic<bool> want_idle_{false};
  std::atomic<bool> running_{false};
  bool live_ = false;
  std::thread thread_;
  std::mutex cb_mutex_;
  std::function<void()> suspend_cb_;
  std::function<void()> resume_cb_;
};

#endif  // DECKBACK_HAVE_SDBUS

}  // namespace

bool Platform::backend_available() { return DECKBACK_HAVE_SDBUS != 0; }

std::unique_ptr<Platform> Platform::create() {
#if DECKBACK_HAVE_SDBUS
  return std::make_unique<SdBusPlatform>();
#else
  warn("platform: no libsystemd — sleep/idle-inhibit disabled (stub backend)");
  return std::make_unique<StubPlatform>();
#endif
}

UnitState user_unit_state(const char* unit) {
#if DECKBACK_HAVE_SDBUS
  sd_bus* bus = nullptr;
  if (sd_bus_open_user(&bus) < 0 || !bus) {
    sd_bus_unref(bus);
    return UnitState::Unknown;
  }
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;
  UnitState out = UnitState::Unknown;
  if (sd_bus_call_method(bus, kSystemd, kSystemdPath, kSystemdManager, "GetUnit", &err, &reply, "s",
                         unit) < 0) {
    if (err.name && std::string_view(err.name) == kNoSuchUnit) out = UnitState::Inactive;
  } else {
    const char* path = nullptr;
    if (sd_bus_message_read(reply, "o", &path) >= 0 && path) {
      sd_bus_error perr = SD_BUS_ERROR_NULL;
      char* state = nullptr;
      if (sd_bus_get_property_string(bus, kSystemd, path, kSystemdUnit, "ActiveState", &perr,
                                     &state) >= 0 &&
          state) {
        const std::string_view sv(state);
        out = (sv == "active" || sv == "activating") ? UnitState::Active : UnitState::Inactive;
        free(state);
      }
      sd_bus_error_free(&perr);
    }
  }
  sd_bus_message_unref(reply);
  sd_bus_error_free(&err);
  sd_bus_unref(bus);
  return out;
#else
  (void)unit;
  return UnitState::Unknown;
#endif
}

}  // namespace deckback
