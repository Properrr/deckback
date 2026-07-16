#include "updater.hpp"

#include <string_view>

#include "log.hpp"

// Gate sd-bus on the header so this compiles without libsystemd (stub backend).
#if __has_include(<systemd/sd-bus.h>)
#define DECKBACK_HAVE_SDBUS 1
#include <fcntl.h>
#include <poll.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include "devtools.hpp"
#include "overlay.hpp"
#include "util.hpp"
#else
#define DECKBACK_HAVE_SDBUS 0
#endif

namespace deckback {

namespace {

constexpr const char* kPermYes = "yes";
constexpr const char* kPermAsk = "ask";
constexpr const char* kPermNo = "no";
constexpr const char* kPermUnsetName = "unset";
constexpr std::string_view kFlatpakInfoAppSection = "[Application]";
constexpr std::string_view kFlatpakInfoNameKey = "name=";

// Reconnect backoff (durable/dbus-reconnect.md). Base 1s, doubling, capped 60s; give up (go inert
// until relaunch) after kReconnectMaxTries consecutive failures.
constexpr std::uint64_t kReconnectBaseMs = 1000;
constexpr std::uint64_t kReconnectMaxMs = 60'000;
constexpr unsigned kReconnectMaxShift = 6;  // 1000<<6 = 64000 -> capped to kReconnectMaxMs
constexpr unsigned kReconnectMaxTries = 12;

}  // namespace

UpdateProgress decode_progress_status(std::uint32_t status) {
  switch (status) {
    case 0:
      return UpdateProgress::Running;
    case 1:
      return UpdateProgress::Empty;
    case 2:
      return UpdateProgress::Done;
    case 3:
      return UpdateProgress::Failed;
    default:
      return UpdateProgress::Unknown;
  }
}

std::uint64_t reconnect_delay_ms(unsigned attempt) {
  const unsigned shift = attempt < kReconnectMaxShift ? attempt : kReconnectMaxShift;
  const std::uint64_t d = kReconnectBaseMs << shift;
  return d < kReconnectMaxMs ? d : kReconnectMaxMs;
}

bool reconnect_should_give_up(unsigned consecutive_failures) {
  return consecutive_failures >= kReconnectMaxTries;
}

UpdatePermission decode_update_permission(const std::vector<std::string>& perms) {
  if (perms.empty()) return UpdatePermission::Unset;
  if (perms.front() == kPermYes) return UpdatePermission::Yes;
  if (perms.front() == kPermAsk) return UpdatePermission::Ask;
  return UpdatePermission::No;
}

const char* update_permission_name(UpdatePermission p) {
  switch (p) {
    case UpdatePermission::Yes:
      return kPermYes;
    case UpdatePermission::Ask:
      return kPermAsk;
    case UpdatePermission::No:
      return kPermNo;
    case UpdatePermission::Unset:
      return kPermUnsetName;
  }
  return kPermUnsetName;
}

void UpdateState::set_available(bool available, std::string commit) {
  {
    std::lock_guard<std::mutex> lk(m_);
    commit_ = std::move(commit);
  }
  available_.store(available, std::memory_order_relaxed);
}

std::string UpdateState::commit() const {
  std::lock_guard<std::mutex> lk(m_);
  return commit_;
}

std::string parse_flatpak_app_id(const std::string& text) {
  bool in_application = false;
  size_t pos = 0;
  while (pos <= text.size()) {
    const size_t eol = text.find('\n', pos);
    std::string_view line(text.data() + pos, (eol == std::string::npos ? text.size() : eol) - pos);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.empty() && line.front() == '[')
      in_application = (line == kFlatpakInfoAppSection);
    else if (in_application && line.starts_with(kFlatpakInfoNameKey))
      return std::string(line.substr(kFlatpakInfoNameKey.size()));
    if (eol == std::string::npos) break;
    pos = eol + 1;
  }
  return {};
}

namespace {

class StubUpdater final : public Updater {
 public:
  void start() override {}
  void stop() override {}
  void request_update() override {}
  bool backend_live() const override { return false; }
  int selftest() override {
    error("selftest-update: no libsystemd backend compiled in");
    return 1;
  }
  int selftest_deploy(bool) override {
    error("selftest-deploy: no libsystemd backend compiled in");
    return 1;
  }
  int selftest_watch(int) override {
    error("selftest-watch: no libsystemd backend compiled in");
    return 1;
  }
};

#if DECKBACK_HAVE_SDBUS

// Set by SIGINT/SIGTERM during --selftest-watch so the bounded watch loop stops promptly and
// cleanly (the sim harness ends the drive with a signal once it has observed the reconnect it was
// testing).
volatile std::sig_atomic_t g_watch_stop = 0;
void watch_on_signal(int) { g_watch_stop = 1; }

constexpr const char* kPortal = "org.freedesktop.portal.Flatpak";
constexpr const char* kPortalPath = "/org/freedesktop/portal/Flatpak";
constexpr const char* kPortalIface = "org.freedesktop.portal.Flatpak";
constexpr const char* kMonitorIface = "org.freedesktop.portal.Flatpak.UpdateMonitor";

// The permission store behind flatpak-portal's self-update consent gate. Seeding
// flatpak/updates=yes is what its own Access dialog would record on "allow" — the dialog Game Mode
// cannot show (durable/self-update.md).
constexpr const char* kPermStore = "org.freedesktop.impl.portal.PermissionStore";
constexpr const char* kPermStorePath = "/org/freedesktop/impl/portal/PermissionStore";
constexpr const char* kPermTable = "flatpak";
constexpr const char* kPermUpdatesId = "updates";
constexpr const char* kFlatpakInfoPath = "/.flatpak-info";

// Bound the synchronous portal calls so a missing/slow portal fails fast instead of hanging on
// sd-bus's 25 s default. These return an ack; deploy progress arrives via signals.
constexpr uint64_t kCallTimeoutUsec = 5'000'000;

// A reconnected session-bus link must stay up at least this long before it "earns back" a fresh
// retry budget; a portal that connects-then-drops faster than this marches toward give-up instead
// of hammering a 1 s reconnect loop (durable/dbus-reconnect.md, review S6).
constexpr long kReconnectStableMs = 30'000;

constexpr uint64_t kUsecPerSec = 1'000'000;
constexpr uint64_t kUsecPerMs = 1000;
constexpr uint64_t kNsecPerUsec = 1000;
constexpr std::string::size_type kShortCommitLen = 12;
constexpr size_t kWakeDrainBytes = 64;

// `--selftest-deploy` waits this long for a terminal Progress, polling at this cadence.
constexpr long kDeploySelftestMs = 90'000;
constexpr int kDeployPollMs = 500;

std::string read_flatpak_app_id() {
  std::ifstream f(kFlatpakInfoPath);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return parse_flatpak_app_id(ss.str());
}

// Wire format of the portal dicts this scans:
//   Progress:        {n_ops:u, op:u, progress:u, status:u, error:s, error_message:s}
//   UpdateAvailable: {running-commit:s, local-commit:s, remote-commit:s}
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
    sd_bus_message_exit_container(m);
  }
  sd_bus_message_exit_container(m);
}

// Session-bus client for the Flatpak update portal. sd-bus is not thread-safe, so every sd-bus call
// happens on one owned event-loop thread. Connect once at start; on any bus/portal failure the
// updater goes inert for the session and the next launch re-checks (durable/self-update.md).
class PortalUpdater final : public Updater {
 public:
  explicit PortalUpdater(UpdaterConfig cfg) : cfg_(std::move(cfg)) {
    if (pipe(wake_pipe_) != 0) {
      warn("updater: pipe() failed — self-update disabled");
      return;
    }
    fcntl(wake_pipe_[0], F_SETFL, O_NONBLOCK);
    if (!open_bus()) {
      warn("updater: sd_bus_open_user failed — self-update disabled");
      return;
    }
    live_ = true;
  }

  ~PortalUpdater() override {
    stop();
    // Slots hold a ref on the bus, so free them before the bus (durable/dbus-reconnect.md, review
    // N10).
    if (slot_avail_) sd_bus_slot_unref(slot_avail_);
    if (slot_progress_) sd_bus_slot_unref(slot_progress_);
    if (slot_owner_) sd_bus_slot_unref(slot_owner_);
    if (wake_pipe_[0] >= 0) close(wake_pipe_[0]);
    if (wake_pipe_[1] >= 0) close(wake_pipe_[1]);
    if (bus_) sd_bus_unref(bus_);
  }

  void start() override {
    if (!live_ || started_) return;
    started_ = true;
    running_.store(true);
    thread_ = std::thread([this] { run(); });
  }

  void stop() override {
    running_.store(false);
    nudge();
    if (thread_.joinable()) thread_.join();
  }

  // Thread-safe consent from the UI thread. Set a flag and wake the loop; the loop (never this
  // caller) issues Update(), so sd_bus_call is never re-entered from outside the event-loop thread.
  void request_update() override {
    confirm_requested_.store(true, std::memory_order_relaxed);
    nudge();
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
    info("selftest-update: permission store " + permission_state());
    return 0;
  }

  int selftest_deploy(bool seed) override {
    if (!live_) {
      error("selftest-deploy: no live session bus");
      return 1;
    }
    if (seed) seed_update_permission();
    info("selftest-deploy: permission store " + permission_state() +
         (seed ? " (after seeding)" : " (raw, not seeded)"));
    if (!create_monitor(&monitor_path_)) {
      error("selftest-deploy: CreateUpdateMonitor failed");
      return 1;
    }
    if (sd_bus_match_signal(bus_, &slot_progress_, kPortal, monitor_path_.c_str(), kMonitorIface,
                            "Progress", &on_progress, this) < 0) {
      error("selftest-deploy: could not subscribe Progress");
      return 1;
    }
    deploy_terminal_ = false;
    if (!do_update()) {
      error("selftest-deploy: the portal rejected Update() (see the warning above)");
      return 3;
    }
    const long deadline = mono_ms() + kDeploySelftestMs;
    while (!deploy_terminal_ && mono_ms() < deadline) {
      if (!pump_pending()) {
        error("selftest-deploy: sd_bus_process error");
        return 1;
      }
      if (deploy_terminal_) break;
      struct pollfd pf {};
      pf.fd = sd_bus_get_fd(bus_);
      pf.events = static_cast<short>(sd_bus_get_events(bus_));
      poll(&pf, 1, kDeployPollMs);
    }
    if (!deploy_terminal_) {
      error("selftest-deploy: no terminal Progress within the deadline");
      return 1;
    }
    switch (deploy_result_) {
      case UpdateProgress::Done:
        info("selftest-deploy: DEPLOYED (status Done) — update applied, binds on next launch");
        return 0;
      case UpdateProgress::Empty:
        info("selftest-deploy: nothing to update (status Empty) — no newer commit on the remote");
        return 2;
      case UpdateProgress::Failed:
        error("selftest-deploy: FAILED (status Failed): " +
              (deploy_err_.empty() ? "unknown error" : deploy_err_));
        return 3;
      default:
        error("selftest-deploy: unexpected terminal state");
        return 1;
    }
  }

  // --selftest-watch <secs>: run the real updater loop (start()) for a bounded window so an
  // external actor can drop the bus or restart flatpak-portal and the reconnect handling logs case
  // A/B. In notify mode with no consent nothing is deployed — the loop only watches. SIGINT/SIGTERM
  // ends the window early (the sim signals once it has seen the reconnect it was testing), which
  // also exercises the stop()/join path. See durable/dbus-reconnect.md, durable/test-sim.md.
  int selftest_watch(int secs) override {
    if (!live_) {
      error("selftest-watch: no live session bus");
      return 1;
    }
    if (secs <= 0) secs = 1;
    info("selftest-watch: running the updater loop for up to " + std::to_string(secs) +
         "s (portal reconnect drive; Ctrl-C / SIGTERM to stop early)");
    g_watch_stop = 0;
    struct sigaction sa {};
    sa.sa_handler = &watch_on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    start();
    const long deadline = mono_ms() + static_cast<long>(secs) * 1000;
    while (mono_ms() < deadline && g_watch_stop == 0) {
      struct timespec ts {
        0, 200L * 1000L * 1000L
      };
      nanosleep(&ts, nullptr);  // EINTR on a signal → the g_watch_stop check ends the loop at once
    }
    info(std::string("selftest-watch: stopping (") +
         (g_watch_stop ? "signalled" : "window elapsed") + ")");
    stop();
    info("selftest-watch: done");
    return 0;
  }

 private:
  // Current updates permission formatted for the selftest logs (one D-Bus round trip).
  std::string permission_state() {
    const std::string app_id = read_flatpak_app_id();
    std::string s = std::string(kPermTable) + "/" + kPermUpdatesId;
    if (app_id.empty()) return s + " — no /.flatpak-info app id";
    return s + " for " + app_id + ": " + update_permission_name(lookup_update_permission(app_id));
  }

  // Lookup(ss) -> (a{sas} v). Any call error (no store, no table, no entry) reads as Unset: nothing
  // has been granted or denied.
  UpdatePermission lookup_update_permission(const std::string& app_id) {
    sd_bus_message* req = nullptr;
    if (sd_bus_message_new_method_call(bus_, &req, kPermStore, kPermStorePath, kPermStore,
                                       "Lookup") < 0)
      return UpdatePermission::Unset;
    sd_bus_message_append(req, "ss", kPermTable, kPermUpdatesId);
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call(bus_, req, kCallTimeoutUsec, &err, &reply);
    sd_bus_message_unref(req);
    sd_bus_error_free(&err);
    if (r < 0) return UpdatePermission::Unset;
    std::vector<std::string> perms;
    if (sd_bus_message_enter_container(reply, 'a', "{sas}") > 0) {
      while (sd_bus_message_enter_container(reply, 'e', "sas") > 0) {
        const char* app = nullptr;
        if (sd_bus_message_read(reply, "s", &app) < 0 || !app) {
          sd_bus_message_exit_container(reply);
          break;
        }
        if (app_id == app && sd_bus_message_enter_container(reply, 'a', "s") > 0) {
          const char* p = nullptr;
          while (sd_bus_message_read(reply, "s", &p) > 0 && p) perms.emplace_back(p);
          sd_bus_message_exit_container(reply);
        } else {
          sd_bus_message_skip(reply, "as");
        }
        sd_bus_message_exit_container(reply);
      }
      sd_bus_message_exit_container(reply);
    }
    sd_bus_message_unref(reply);
    return decode_update_permission(perms);
  }

  bool grant_update_permission(const std::string& app_id) {
    sd_bus_message* req = nullptr;
    if (sd_bus_message_new_method_call(bus_, &req, kPermStore, kPermStorePath, kPermStore,
                                       "SetPermission") < 0)
      return false;
    sd_bus_message_append(req, "sbss", kPermTable, 1, kPermUpdatesId, app_id.c_str());
    sd_bus_message_open_container(req, 'a', "s");
    sd_bus_message_append(req, "s", kPermYes);
    sd_bus_message_close_container(req);
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call(bus_, req, kCallTimeoutUsec, &err, &reply);
    sd_bus_message_unref(req);
    if (r < 0) {
      warn(std::string("updater: SetPermission: ") + (err.message ? err.message : "error"));
      sd_bus_error_free(&err);
      return false;
    }
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    return true;
  }

  // An explicit host-side "no" wins: the watcher still runs so the refusal is logged, but no
  // consent is forged over it.
  // Returns true when consent is DETERMINATE (nothing to retry): not sandboxed, already granted or
  // denied, or a fresh grant succeeded. Returns false ONLY when a grant was attempted and the call
  // itself failed (e.g. the permission store was unreachable at bring-up) — a transient error the
  // caller must NOT latch, so a later reconnect re-seeds instead of leaving auto-deploy silently
  // un-consented (review SF2).
  bool seed_update_permission() {
    const std::string app_id = read_flatpak_app_id();
    if (app_id.empty()) {
      info("updater: no /.flatpak-info app id — skipping the consent pre-grant");
      return true;
    }
    switch (lookup_update_permission(app_id)) {
      case UpdatePermission::Yes:
        return true;
      case UpdatePermission::No:
        warn("updater: the permission store denies self-update for " + app_id +
             " — the portal will refuse to deploy ('flatpak permission-set " + kPermTable + " " +
             kPermUpdatesId + " " + app_id + " yes' on the host to allow)");
        return true;
      case UpdatePermission::Unset:
      case UpdatePermission::Ask:
        // Don't start the second blocking sd_bus_call after a stop() landed (bounds join latency to
        // one in-flight ~5 s call, review SF3).
        if (!running_.load()) return false;
        if (grant_update_permission(app_id)) {
          info("updater: pre-granted self-update consent (" + std::string(kPermTable) + "/" +
               kPermUpdatesId + "=yes) — Game Mode has no Access portal for the consent dialog");
          return true;
        }
        warn(
            "updater: could not pre-grant self-update consent — deploys will fail where no "
            "Access portal can show the consent dialog (Game Mode); will retry on reconnect");
        return false;
    }
    return true;
  }

  bool create_monitor(std::string* out_path) {
    sd_bus_message* req = nullptr;
    if (sd_bus_message_new_method_call(bus_, &req, kPortal, kPortalPath, kPortalIface,
                                       "CreateUpdateMonitor") < 0)
      return false;
    sd_bus_message_open_container(req, 'a', "{sv}");
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

  // (Re)open the session bus. Idempotent: a live bus is left as-is.
  bool open_bus() {
    if (bus_) return true;
    if (sd_bus_open_user(&bus_) < 0 || !bus_) {
      bus_ = nullptr;  // sd_bus_open_user may set a half-constructed handle on failure
      return false;
    }
    return true;
  }

  // Drop only the UpdateMonitor (its two signal slots + path); keep the bus and the owner-change
  // match. An in-flight deploy died with the monitor, so clear updating_ — but NOT want_update_: a
  // latched user "Update now" consent must survive a rebuild (durable/dbus-reconnect.md, review
  // S5).
  void teardown_monitor() {
    if (slot_avail_) {
      sd_bus_slot_unref(slot_avail_);
      slot_avail_ = nullptr;
    }
    if (slot_progress_) {
      sd_bus_slot_unref(slot_progress_);
      slot_progress_ = nullptr;
    }
    monitor_path_.clear();
    updating_ = false;
  }

  // Drop the whole connection: monitor slots, the owner-change match, then the bus (slots first —
  // they hold a bus ref, review N10).
  void teardown_bus() {
    teardown_monitor();
    if (slot_owner_) {
      sd_bus_slot_unref(slot_owner_);
      slot_owner_ = nullptr;
    }
    if (bus_) {
      sd_bus_unref(bus_);
      bus_ = nullptr;
    }
  }

  // Create the UpdateMonitor and subscribe its two signals. Used at bring-up and to rebuild the
  // monitor after a portal restart (case B). teardown_monitor() first so a rebuild can't leak
  // slots.
  bool establish_monitor() {
    teardown_monitor();
    if (!create_monitor(&monitor_path_)) return false;
    if (sd_bus_match_signal(bus_, &slot_avail_, kPortal, monitor_path_.c_str(), kMonitorIface,
                            "UpdateAvailable", &on_update_available, this) < 0)
      return false;
    if (sd_bus_match_signal(bus_, &slot_progress_, kPortal, monitor_path_.c_str(), kMonitorIface,
                            "Progress", &on_progress, this) < 0)
      return false;
    return true;
  }

  // Watch for flatpak-portal (re)acquiring its bus name = it restarted, so our monitor object is
  // orphaned and must be rebuilt (case B). arg0 in the match limits the wakeups to the portal;
  // sd_bus_match_signal has no arg0 filter, so an explicit match string is required (review S3).
  // Subscribed AFTER establish_monitor() so the portal's own activation NameOwnerChanged (emitted
  // while create_monitor() activates it) is not mistaken for a restart.
  bool subscribe_portal_owner_changes() {
    if (slot_owner_) return true;
    static constexpr const char* kOwnerMatch =
        "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
        "member='NameOwnerChanged',arg0='org.freedesktop.portal.Flatpak'";
    return sd_bus_add_match(bus_, &slot_owner_, kOwnerMatch, &on_name_owner_changed, this) >= 0;
  }

  // Bring the whole watch up: open the bus, seed consent ONCE per session (reconnects skip it — it
  // is a blocking call and re-writing the permission store every reconnect is wrong, review C1/S6),
  // then the monitor and the owner-change match. running_ is re-checked before each blocking step
  // so a stop() that lands mid-establish is honored promptly (worst case: one in-flight ~5 s
  // sd_bus_call).
  bool establish() {
    if (!running_.load() || !open_bus()) return false;
    if (!seeded_) {
      if (!running_.load()) return false;
      // Seed consent in every mode: a user-confirmed notify deploy needs the same no-dialog path as
      // an auto deploy (durable/self-update.md ★ SOLUTION); an explicit host "no" is still
      // respected. Latch ONLY a determinate outcome so a transient failure re-seeds on reconnect
      // (SF2).
      if (seed_update_permission()) seeded_ = true;
    }
    if (!running_.load() || !establish_monitor() || !subscribe_portal_owner_changes()) {
      teardown_bus();
      return false;
    }
    return true;
  }

  // Interruptible backoff: sleep up to ms, but wake at once on a stop() nudge. Returns running_ so
  // the retry loop exits immediately on stop (review C2 — also what keeps the L0 lifecycle fast).
  bool wait_backoff(std::uint64_t ms) {
    struct pollfd pf {};
    pf.fd = wake_pipe_[0];
    pf.events = POLLIN;
    const int to = ms > static_cast<std::uint64_t>(INT_MAX) ? INT_MAX : static_cast<int>(ms);
    poll(&pf, 1, to);
    if (pf.revents & POLLIN) drain_wake();
    return running_.load();
  }

  // establish() with bounded exponential backoff. Success stamps last_connect_ms_; after
  // kReconnectMaxTries consecutive failures the watcher goes inert until the next launch (the
  // connect-once floor, just deferred). Returns false on give-up OR a stop().
  bool establish_with_retry() {
    while (running_.load()) {
      if (establish()) {
        last_connect_ms_ = mono_ms();
        return true;
      }
      if (reconnect_should_give_up(++reconnect_failures_)) {
        warn(
            "updater: could not reach the Flatpak portal after repeated tries — self-update "
            "inactive until next launch");
        return false;
      }
      const std::uint64_t delay = reconnect_delay_ms(reconnect_failures_ - 1);
      warn("updater: update monitor unavailable (attempt " + std::to_string(reconnect_failures_) +
           ") — retrying in " + std::to_string(delay / 1000) + "s");
      if (!wait_backoff(delay)) return false;
    }
    return false;
  }

  void run() {
    if (establish_with_retry()) {
      info("updater: watching for updates to this app via the Flatpak portal");
      loop();
    }
    running_.store(false);  // uniform post-condition on every loop/give-up exit (review N1)
  }

  // Drain all pending sd-bus work; false if the connection errored.
  bool pump_pending() {
    for (;;) {
      int r = sd_bus_process(bus_, nullptr);
      if (r < 0) return false;
      if (r == 0) return true;
    }
  }

  void loop() {
    reconnect_failures_ = 0;  // a fresh, healthy connection: full retry budget for the first drop
    while (running_.load()) {
      if (!pump_pending()) {
        // Case A: our session-bus connection dropped. Reconnect instead of going inert until the
        // next launch (durable/dbus-reconnect.md). A link that stayed up a while earns a fresh
        // budget; a flapping one keeps its climbing failure count and marches toward give-up
        // (review S6).
        const bool was_stable = (mono_ms() - last_connect_ms_) >= kReconnectStableMs;
        warn("updater: session bus error — attempting to reconnect");
        teardown_bus();
        if (was_stable) {
          reconnect_failures_ = 0;  // a long-lived link earns a fresh retry budget
        } else if (reconnect_should_give_up(++reconnect_failures_)) {
          // A link that keeps dropping almost immediately is flapping — count it and, once the
          // budget is spent, go inert instead of spinning a tight reconnect loop (review SF1).
          warn(
              "updater: the Flatpak portal connection keeps dropping — self-update inactive "
              "until next launch");
          return;
        } else if (!wait_backoff(reconnect_delay_ms(reconnect_failures_ - 1))) {
          return;  // stop() during the flap backoff
        }
        if (!establish_with_retry()) return;  // exhausted or stopped -> inert (review S7)
        info("updater: reconnected to the Flatpak portal — watching again");
        continue;
      }
      if (!running_.load()) break;
      if (reestablish_monitor_) {
        // Case B: flatpak-portal restarted (NameOwnerChanged), so our monitor object is orphaned.
        // The bus is fine — rebuild only the monitor. A failed rebuild falls into the reconnect
        // path.
        reestablish_monitor_ = false;
        info("updater: flatpak-portal restarted — re-creating the update monitor");
        if (establish_monitor()) {
          last_connect_ms_ = mono_ms();
        } else {
          warn("updater: could not re-create the monitor after the portal restart — reconnecting");
          teardown_bus();
          if (!establish_with_retry()) return;
        }
        continue;
      }
      // A user "Update now" (notify mode) lands here, on the loop thread, so Update() is issued the
      // same way an auto deploy is — never from the request_update() caller's thread.
      if (confirm_requested_.exchange(false, std::memory_order_relaxed) && !updating_) {
        want_update_ = true;
      }
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

  // Ask the portal to deploy. Returns false if the Update() call itself was rejected synchronously
  // (so callers need not wait for a Progress signal that will never arrive); true if it was
  // accepted and the deploy proceeds in the background, reported later via Progress.
  bool do_update() {
    sd_bus_message* req = nullptr;
    if (sd_bus_message_new_method_call(bus_, &req, kPortal, monitor_path_.c_str(), kMonitorIface,
                                       "Update") < 0) {
      updating_ = false;
      return false;
    }
    sd_bus_message_append(req, "s", "");
    sd_bus_message_open_container(req, 'a', "{sv}");
    sd_bus_message_close_container(req);
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call(bus_, req, kCallTimeoutUsec, &err, &reply);
    sd_bus_message_unref(req);
    if (r < 0) {
      warn(std::string("updater: Update(): ") + (err.message ? err.message : "error"));
      sd_bus_error_free(&err);
      updating_ = false;
      return false;
    }
    sd_bus_error_free(&err);
    info("updater: update requested; deploying in the background");
    sd_bus_message_unref(reply);
    return true;
  }

  void announce_ready() {
    if (cfg_.cdp_port <= 0) return;
    DevToolsClient client(cfg_.cdp_host, cfg_.cdp_port);
    show_toast(client, cfg_.toast_text, cfg_.toast_ms);
  }

  // NameOwnerChanged(name, old_owner, new_owner) for the portal. A non-empty new owner = the portal
  // (re)appeared, i.e. it restarted and our monitor is orphaned. Runs on the loop thread (inside
  // sd_bus_process), so it only sets a plain flag; the loop rebuilds the monitor — never from here,
  // to keep sd_bus_call out of sd_bus_process (review N8).
  static int on_name_owner_changed(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<PortalUpdater*>(userdata);
    const char* name = nullptr;
    const char* old_owner = nullptr;
    const char* new_owner = nullptr;
    if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0) return 0;
    if (new_owner && *new_owner) self->reestablish_monitor_ = true;
    return 0;
  }

  static int on_update_available(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<PortalUpdater*>(userdata);
    std::string remote;
    scan_dict(m, nullptr, nullptr, "remote-commit", &remote);
    const std::string short_commit = remote.substr(0, kShortCommitLen);
    info("updater: an update is available" +
         (remote.empty() ? std::string() : " (remote " + short_commit + ")"));
    if (self->cfg_.auto_deploy) {
      if (!self->updating_) self->want_update_ = true;  // deploy now (auto mode)
    } else {
      // notify mode: publish availability for the UI threads; the deploy waits for
      // request_update().
      if (self->cfg_.state) self->cfg_.state->set_available(true, short_commit);
      info("updater: notify mode — awaiting user confirmation in Settings ▸ Updates");
    }
    return 0;
  }

  static int on_progress(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<PortalUpdater*>(userdata);
    uint32_t status = 0;
    std::string errmsg;
    scan_dict(m, "status", &status, "error_message", &errmsg);
    const UpdateProgress p = decode_progress_status(status);
    switch (p) {
      case UpdateProgress::Done:
        info("updater: update deployed — it will apply the next time Deckback is launched");
        if (self->updating_) self->announce_ready();
        self->updating_ = false;
        self->record_terminal(p, {});
        break;
      case UpdateProgress::Failed:
        warn("updater: update failed: " + (errmsg.empty() ? "unknown error" : errmsg));
        self->updating_ = false;
        self->record_terminal(p, errmsg);
        break;
      case UpdateProgress::Empty:
        self->updating_ = false;
        self->record_terminal(p, {});
        break;
      case UpdateProgress::Running:
      case UpdateProgress::Unknown:
        break;
    }
    return 0;
  }

  // Latch the first terminal Progress for the bounded --selftest-deploy loop. Unused by the normal
  // watcher (loop() never reads these).
  void record_terminal(UpdateProgress p, std::string err) {
    deploy_result_ = p;
    deploy_err_ = std::move(err);
    deploy_terminal_ = true;
  }

  int loop_timeout_ms() {
    uint64_t until = 0;
    int r = sd_bus_get_timeout(bus_, &until);
    if (r < 0 || until == UINT64_MAX) return -1;
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = static_cast<uint64_t>(ts.tv_sec) * kUsecPerSec + ts.tv_nsec / kNsecPerUsec;
    if (until <= now) return 0;
    uint64_t ms = (until - now + (kUsecPerMs - 1)) / kUsecPerMs;
    return ms > static_cast<uint64_t>(INT_MAX) ? INT_MAX : static_cast<int>(ms);
  }

  void nudge() {
    if (wake_pipe_[1] >= 0) {
      const char b = 1;
      ssize_t n = write(wake_pipe_[1], &b, 1);
      (void)n;
    }
  }
  void drain_wake() {
    char buf[kWakeDrainBytes];
    while (read(wake_pipe_[0], buf, sizeof buf) > 0) {
    }
  }

  UpdaterConfig cfg_;
  sd_bus* bus_ = nullptr;
  sd_bus_slot* slot_avail_ = nullptr;
  sd_bus_slot* slot_progress_ = nullptr;
  sd_bus_slot* slot_owner_ =
      nullptr;  // portal NameOwnerChanged match (case B, durable/dbus-reconnect.md)
  std::string monitor_path_;
  int wake_pipe_[2] = {-1, -1};
  std::atomic<bool> running_{false};
  bool started_ = false;
  std::atomic<bool> confirm_requested_{false};  // set by request_update() (UI thread), read by loop
  bool want_update_ = false;
  bool updating_ = false;
  bool seeded_ = false;  // consent seeded once per session (reconnects skip it)
  bool reestablish_monitor_ =
      false;  // set by on_name_owner_changed, acted on in loop() (loop thread)
  unsigned reconnect_failures_ = 0;  // consecutive failed (re)connects; reset by a stable link
  long last_connect_ms_ = 0;         // mono_ms() of the last successful establish (stability gate)
  bool live_ = false;
  bool deploy_terminal_ = false;
  UpdateProgress deploy_result_ = UpdateProgress::Unknown;
  std::string deploy_err_;
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
