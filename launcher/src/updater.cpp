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
  bool backend_live() const override { return false; }
  int selftest() override {
    error("selftest-update: no libsystemd backend compiled in");
    return 1;
  }
  int selftest_deploy(bool) override {
    error("selftest-deploy: no libsystemd backend compiled in");
    return 1;
  }
};

#if DECKBACK_HAVE_SDBUS

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
    if (sd_bus_open_user(&bus_) < 0 || !bus_) {
      warn("updater: sd_bus_open_user failed — self-update disabled");
      return;
    }
    if (pipe(wake_pipe_) != 0) {
      warn("updater: pipe() failed — self-update disabled");
      return;
    }
    fcntl(wake_pipe_[0], F_SETFL, O_NONBLOCK);
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
  void seed_update_permission() {
    const std::string app_id = read_flatpak_app_id();
    if (app_id.empty()) {
      info("updater: no /.flatpak-info app id — skipping the consent pre-grant");
      return;
    }
    switch (lookup_update_permission(app_id)) {
      case UpdatePermission::Yes:
        return;
      case UpdatePermission::No:
        warn("updater: the permission store denies self-update for " + app_id +
             " — the portal will refuse to deploy ('flatpak permission-set " + kPermTable + " " +
             kPermUpdatesId + " " + app_id + " yes' on the host to allow)");
        return;
      case UpdatePermission::Unset:
      case UpdatePermission::Ask:
        if (grant_update_permission(app_id))
          info("updater: pre-granted self-update consent (" + std::string(kPermTable) + "/" +
               kPermUpdatesId + "=yes) — Game Mode has no Access portal for the consent dialog");
        else
          warn(
              "updater: could not pre-grant self-update consent — deploys will fail where no "
              "Access portal can show the consent dialog (Game Mode)");
        return;
    }
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

  void run() {
    if (cfg_.enabled) seed_update_permission();
    if (!create_monitor(&monitor_path_)) {
      error("updater: could not create the update monitor — self-update inactive this session");
      running_.store(false);
      return;
    }
    int ra = sd_bus_match_signal(bus_, &slot_avail_, kPortal, monitor_path_.c_str(), kMonitorIface,
                                 "UpdateAvailable", &on_update_available, this);
    int rp = sd_bus_match_signal(bus_, &slot_progress_, kPortal, monitor_path_.c_str(),
                                 kMonitorIface, "Progress", &on_progress, this);
    if (ra < 0 || rp < 0) {
      warn("updater: could not subscribe portal signals — self-update inactive this session");
      running_.store(false);
      return;
    }
    info("updater: watching for updates to this app via the Flatpak portal");
    loop();
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
    while (running_.load()) {
      if (!pump_pending()) {
        warn("updater: sd_bus_process error — self-update inactive until next launch");
        return;
      }
      if (!running_.load()) break;
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

  static int on_update_available(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<PortalUpdater*>(userdata);
    std::string remote;
    scan_dict(m, nullptr, nullptr, "remote-commit", &remote);
    info("updater: an update is available" +
         (remote.empty() ? std::string() : " (remote " + remote.substr(0, kShortCommitLen) + ")"));
    if (self->cfg_.enabled && !self->updating_)
      self->want_update_ = true;
    else if (!self->cfg_.enabled)
      info("updater: self_update is off — not applying (update manually or enable self_update)");
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
  std::string monitor_path_;
  int wake_pipe_[2] = {-1, -1};
  std::atomic<bool> running_{false};
  bool started_ = false;
  bool want_update_ = false;
  bool updating_ = false;
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
