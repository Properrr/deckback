#include "updater.hpp"

#include <initializer_list>
#include <string_view>
#include <utility>

#include "busloop.hpp"
#include "fileio.hpp"
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
constexpr const char* kFlatpakInfoPath = "/.flatpak-info";

// flatpak-portal's refusal of a permission-widening update: the encoded G_DBUS_ERROR_NOT_SUPPORTED,
// plus a substring of the untranslated message as a fallback.
constexpr std::string_view kNotSupportedError = "org.freedesktop.DBus.Error.NotSupported";
constexpr std::string_view kPermissionMessageNeedle = "new permissions";

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

bool is_permission_change_failure(std::string_view error_name, std::string_view error_message) {
  return error_name == kNotSupportedError ||
         error_message.find(kPermissionMessageNeedle) != std::string_view::npos;
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

std::string parse_flatpak_info_value(const std::string& text, std::string_view section,
                                     std::string_view key) {
  bool in_section = false;
  size_t pos = 0;
  while (pos <= text.size()) {
    const size_t eol = text.find('\n', pos);
    std::string_view line(text.data() + pos, (eol == std::string::npos ? text.size() : eol) - pos);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.empty() && line.front() == '[') {
      in_section = line.size() == section.size() + 2 && line.back() == ']' &&
                   line.substr(1, section.size()) == section;
    } else if (in_section && line.size() > key.size() && line.starts_with(key) &&
               line[key.size()] == '=') {
      return std::string(line.substr(key.size() + 1));
    }
    if (eol == std::string::npos) break;
    pos = eol + 1;
  }
  return {};
}

std::string parse_flatpak_app_id(const std::string& text) {
  return parse_flatpak_info_value(text, "Application", "name");
}

std::string read_running_commit() {
  const auto text = read_file(kFlatpakInfoPath);
  return text ? parse_flatpak_info_value(*text, "Instance", "app-commit") : std::string();
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
};

#if DECKBACK_HAVE_SDBUS

constexpr const char* kPortal = "org.freedesktop.portal.Flatpak";
constexpr const char* kPortalPath = "/org/freedesktop/portal/Flatpak";
constexpr const char* kPortalIface = "org.freedesktop.portal.Flatpak";
constexpr const char* kMonitorIface = "org.freedesktop.portal.Flatpak.UpdateMonitor";

// The permission store behind flatpak-portal's self-update consent. flatpak/updates=yes is what its
// Access dialog would record on "allow".
constexpr const char* kPermStore = "org.freedesktop.impl.portal.PermissionStore";
constexpr const char* kPermStorePath = "/org/freedesktop/impl/portal/PermissionStore";
constexpr const char* kPermTable = "flatpak";
constexpr const char* kPermUpdatesId = "updates";

// Bound the synchronous portal calls so a missing/slow portal fails fast instead of hanging on
// sd-bus's 25 s default. These return an ack; deploy progress arrives via signals.
constexpr uint64_t kCallTimeoutUsec = 5'000'000;

// `--selftest-deploy` waits this long for a terminal Progress, polling at this cadence.
constexpr long kDeploySelftestMs = 90'000;
constexpr int kDeployPollMs = 500;

std::string read_flatpak_app_id() {
  const auto text = read_file(kFlatpakInfoPath);
  return text ? parse_flatpak_app_id(*text) : std::string();
}

// Reads the wanted keys of an a{sv} portal dict in one pass (a message can only be read once):
//   Progress:        {n_ops:u, op:u, progress:u, status:u, error:s, error_message:s}
//   UpdateAvailable: {running-commit:s, local-commit:s, remote-commit:s}
void scan_dict(sd_bus_message* m, std::initializer_list<std::pair<const char*, uint32_t*>> uints,
               std::initializer_list<std::pair<const char*, std::string*>> strs) {
  if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return;
  while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
    const char* key = nullptr;
    if (sd_bus_message_read(m, "s", &key) < 0 || !key) {
      sd_bus_message_exit_container(m);
      break;
    }
    std::string* want_str = nullptr;
    for (const auto& [name, target] : strs)
      if (std::strcmp(key, name) == 0) want_str = target;
    uint32_t* want_u = nullptr;
    for (const auto& [name, target] : uints)
      if (std::strcmp(key, name) == 0) want_u = target;

    if (want_u) {
      if (sd_bus_message_enter_container(m, 'v', "u") > 0) {
        sd_bus_message_read(m, "u", want_u);
        sd_bus_message_exit_container(m);
      } else {
        sd_bus_message_skip(m, "v");
      }
    } else if (want_str) {
      const char* s = nullptr;
      if (sd_bus_message_enter_container(m, 'v', "s") > 0) {
        if (sd_bus_message_read(m, "s", &s) >= 0 && s) *want_str = s;
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
// happens on one owned event-loop thread. A dropped bus or restarted portal is reconnected.
class PortalUpdater final : public Updater {
 public:
  explicit PortalUpdater(UpdaterConfig cfg) : cfg_(std::move(cfg)) {
    if (!wake_.ok()) {
      warn("updater: pipe() failed — self-update disabled");
      return;
    }
    if (!open_bus()) {
      warn("updater: sd_bus_open_user failed — self-update disabled");
      return;
    }
    live_ = true;
  }

  ~PortalUpdater() override {
    stop();
    // Slots hold a ref on the bus, so free them before the bus.
    if (slot_avail_) sd_bus_slot_unref(slot_avail_);
    if (slot_progress_) sd_bus_slot_unref(slot_progress_);
    if (slot_owner_) sd_bus_slot_unref(slot_owner_);
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

  // Sets a flag and wakes the loop, which issues Update(): sd-bus is never touched from the
  // caller's thread.
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
      struct pollfd pf{};
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

  // Pre-grant self-update consent unless the store already says yes or no (an explicit host "no"
  // wins). Returns true when the outcome is determinate; false only when a grant was attempted and
  // the call failed, a transient error the caller must not latch so a reconnect re-seeds.
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
        // Don't start a second blocking call after stop().
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

  // Drop only the UpdateMonitor (its signal slots + path), keeping the bus and the owner-change
  // match. Clears updating_ (the deploy died with the monitor) but NOT want_update_: a latched user
  // consent must survive a rebuild.
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

  // Drop the whole connection; slots before the bus.
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

  // Create the UpdateMonitor and subscribe its signals; also rebuilds it after a portal restart.
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

  // flatpak-portal (re)acquiring its bus name means it restarted and orphaned our monitor. Needs an
  // explicit match string: sd_bus_match_signal has no arg0 filter. Subscribe AFTER
  // establish_monitor() so the portal's own activation is not taken for a restart.
  bool subscribe_portal_owner_changes() {
    if (slot_owner_) return true;
    static constexpr const char* kOwnerMatch =
        "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
        "member='NameOwnerChanged',arg0='org.freedesktop.portal.Flatpak'";
    return sd_bus_add_match(bus_, &slot_owner_, kOwnerMatch, &on_name_owner_changed, this) >= 0;
  }

  // Open the bus, seed consent once per session (reconnects skip it), then the monitor and the
  // owner-change match. running_ is re-checked before each blocking step so a stop() is honored
  // within one in-flight call.
  bool establish() {
    if (!running_.load() || !open_bus()) return false;
    if (!seeded_) {
      if (!running_.load()) return false;
      // Seeded in every mode: a confirmed notify deploy needs the same no-dialog path as auto.
      if (seed_update_permission()) seeded_ = true;
    }
    if (!running_.load() || !establish_monitor() || !subscribe_portal_owner_changes()) {
      teardown_bus();
      return false;
    }
    return true;
  }

  // Sleep up to ms, waking at once on a stop() nudge. Returns running_.
  bool wait_backoff(std::uint64_t ms) {
    wake_.wait(ms);
    return running_.load();
  }

  // establish() with exponential backoff until reconnect_should_give_up. Returns false on give-up
  // or stop().
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
    running_.store(false);  // on every exit path
  }

  // Drain all pending sd-bus work; false if the connection errored.
  bool pump_pending() { return bus_pump(bus_); }

  void loop() {
    reconnect_failures_ = 0;  // a fresh, healthy connection: full retry budget for the first drop
    while (running_.load()) {
      if (!pump_pending()) {
        // The bus dropped. A link that stayed up kReconnectStableMs earns a fresh retry budget; a
        // flapping one keeps counting toward give-up.
        const bool was_stable = (mono_ms() - last_connect_ms_) >= kReconnectStableMs;
        warn("updater: session bus error — attempting to reconnect");
        teardown_bus();
        if (was_stable) {
          reconnect_failures_ = 0;
        } else if (reconnect_should_give_up(++reconnect_failures_)) {
          warn(
              "updater: the Flatpak portal connection keeps dropping — self-update inactive "
              "until next launch");
          return;
        } else if (!wait_backoff(reconnect_delay_ms(reconnect_failures_ - 1))) {
          return;  // stop() during the flap backoff
        }
        if (!establish_with_retry()) return;  // exhausted or stopped -> inert
        info("updater: reconnected to the Flatpak portal — watching again");
        continue;
      }
      if (!running_.load()) break;
      if (reestablish_monitor_) {
        // flatpak-portal restarted: the bus is fine, so rebuild only the monitor; a failed rebuild
        // reconnects.
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
      // Consent from request_update(), acted on here so Update() only runs on the loop thread.
      if (confirm_requested_.exchange(false, std::memory_order_relaxed) && !updating_) {
        want_update_ = true;
      }
      if (want_update_ && !updating_) {
        want_update_ = false;
        updating_ = true;
        do_update();
      }
      bus_wait(bus_, wake_);
    }
  }

  // Ask the portal to deploy. False if Update() was rejected synchronously (no Progress will
  // follow); true if accepted, with the outcome reported later via Progress.
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
      const std::string msg = err.message ? err.message : "error";
      warn("updater: Update(): " + msg);
      // A refused call is a terminal verdict: no Progress signal will follow.
      record_terminal(UpdateProgress::Failed, msg);
      if (cfg_.on_terminal) cfg_.on_terminal(UpdateProgress::Failed, err.name ? err.name : "", msg);
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

  // NameOwnerChanged(name, old_owner, new_owner) for the portal: a non-empty new owner means it
  // restarted. Runs inside sd_bus_process, so it only sets a flag; the loop rebuilds the monitor.
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
    scan_dict(m, {}, {{"remote-commit", &remote}});
    const std::string short_remote = short_commit(remote);
    info("updater: an update is available" +
         (remote.empty() ? std::string() : " (remote " + short_remote + ")"));
    if (self->cfg_.auto_deploy) {
      if (!self->updating_) self->want_update_ = true;  // deploy now (auto mode)
    } else {
      // notify mode: publish availability; the deploy waits for request_update().
      if (self->cfg_.state) self->cfg_.state->set_available(true, short_remote);
      info("updater: notify mode — awaiting user confirmation in Settings ▸ Updates");
    }
    return 0;
  }

  static int on_progress(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<PortalUpdater*>(userdata);
    uint32_t status = 0, op = 0, n_ops = 0, percent = 0;
    std::string errname, errmsg;
    scan_dict(m, {{"status", &status}, {"op", &op}, {"n_ops", &n_ops}, {"progress", &percent}},
              {{"error", &errname}, {"error_message", &errmsg}});
    const UpdateProgress p = decode_progress_status(status);
    if (p == UpdateProgress::Running) {
      if (self->cfg_.on_running) self->cfg_.on_running(op, n_ops, percent);
      return 0;
    }
    if (p == UpdateProgress::Unknown) return 0;

    const bool was_updating = self->updating_;
    self->updating_ = false;
    switch (p) {
      case UpdateProgress::Done:
        info("updater: update deployed — it will apply the next time Deckback is launched");
        break;
      case UpdateProgress::Failed:
        warn("updater: update failed: " + (errmsg.empty() ? "unknown error" : errmsg) +
             (errname.empty() ? "" : " [" + errname + "]"));
        if (is_permission_change_failure(errname, errmsg))
          warn(
              "updater: this build asks for a sandbox permission the installed one lacks, so the "
              "portal will never deploy it — the user must update once from the host "
              "(scripts/check-permissions.sh exists to stop this reaching a release)");
        break;
      case UpdateProgress::Empty:
        info(
            "updater: nothing to deploy — the installed commit is already the newest on the "
            "remote");
        break;
      case UpdateProgress::Running:
      case UpdateProgress::Unknown:
        break;
    }
    self->record_terminal(p, errmsg);
    // A set on_terminal owns the user-visible message; otherwise toast a completed deploy here.
    if (self->cfg_.on_terminal)
      self->cfg_.on_terminal(p, errname, errmsg);
    else if (p == UpdateProgress::Done && was_updating)
      self->announce_ready();
    return 0;
  }

  // Terminal Progress for --selftest-deploy; the normal watcher never reads these.
  void record_terminal(UpdateProgress p, std::string err) {
    deploy_result_ = p;
    deploy_err_ = std::move(err);
    deploy_terminal_ = true;
  }

  void nudge() { wake_.nudge(); }

  UpdaterConfig cfg_;
  sd_bus* bus_ = nullptr;
  sd_bus_slot* slot_avail_ = nullptr;
  sd_bus_slot* slot_progress_ = nullptr;
  sd_bus_slot* slot_owner_ = nullptr;  // portal NameOwnerChanged match
  std::string monitor_path_;
  WakePipe wake_;  // stop()/request_update() nudge the loop thread
  std::atomic<bool> running_{false};
  bool started_ = false;
  std::atomic<bool> confirm_requested_{false};  // set by request_update() (UI thread), read by loop
  bool want_update_ = false;
  bool updating_ = false;
  bool seeded_ = false;               // consent seeded once per session (reconnects skip it)
  bool reestablish_monitor_ = false;  // set by on_name_owner_changed, handled in loop()
  unsigned reconnect_failures_ = 0;   // consecutive failed (re)connects; reset by a stable link
  long last_connect_ms_ = 0;          // mono_ms() of the last successful establish (stability gate)
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
