#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "busloop.hpp"  // reconnect_delay_ms, reconnect_should_give_up

namespace deckback {

// Thread-safe hand-off of "an update is available" from the updater thread to the input thread.
// Keyed on the portal's short remote ostree commit; version labels are cosmetic to the portal.
class UpdateState {
 public:
  // Updater thread. Dismissal is not stored here: it is keyed to the commit by an on-disk marker
  // (decide_notification), so a newer commit re-arms the indicator.
  void set_available(bool available, std::string commit);
  bool available() const { return available_.load(std::memory_order_relaxed); }
  std::string commit() const;  // the short remote commit, mutex-guarded

 private:
  std::atomic<bool> available_{false};
  mutable std::mutex m_;
  std::string commit_;
};

// Flatpak portal `Progress.status` codes: 0 Running, 1 Empty (nothing to do), 2 Done, 3 Failed.
enum class UpdateProgress { Running, Empty, Done, Failed, Unknown };
UpdateProgress decode_progress_status(std::uint32_t status);

// True when a Failed Progress means the new build asks for a permission the installed one lacks.
// flatpak-portal always refuses such an update (G_DBUS_ERROR_NOT_SUPPORTED); only an update run
// from the host can install it. Keyed on the D-Bus error name because the message is translated;
// the message substring is a fallback.
bool is_permission_change_failure(std::string_view error_name, std::string_view error_message);

// flatpak-portal's self-update consent in the permission store (table "flatpak", id "updates"):
// "yes" deploys silently; anything else needs an Access-portal dialog that Game Mode cannot show.
// Mirrors flatpak's get_update_permission(): only the first element counts, "yes"/"ask" match
// exactly, any other value is No, no entry is Unset.
enum class UpdatePermission { Unset, Ask, Yes, No };
UpdatePermission decode_update_permission(const std::vector<std::string>& perms);
const char* update_permission_name(UpdatePermission p);

// The value of `key` inside `[section]` of /.flatpak-info text (keyfile format); empty when absent.
// `section` is given without brackets. Pure.
std::string parse_flatpak_info_value(const std::string& text, std::string_view section,
                                     std::string_view key);
// App id from /.flatpak-info text (`[Application]` section, `name=` key); empty when not sandboxed.
std::string parse_flatpak_app_id(const std::string& text);
// The ostree commit this process was started from (`[Instance] app-commit` in /.flatpak-info), or
// "" outside the sandbox. A deploy staged during this session does not change it.
std::string read_running_commit();

// Commits are keyed by their first 12 hex digits wherever the UI and the dedup markers see them;
// every source must shorten the same way or one commit would read as two.
inline constexpr std::size_t kShortCommitLen = 12;
inline std::string short_commit(std::string_view commit) {
  return std::string(commit.substr(0, kShortCommitLen));
}

struct UpdaterConfig {
  // true (auto): deploy on detection. false (notify): only publish availability and wait for
  // request_update(). In `off` mode no Updater is constructed.
  bool auto_deploy = false;
  // Receives availability for the UI threads. Not owned; must outlive the Updater. May be null.
  UpdateState* state = nullptr;
  // For the "restart to apply" toast only; cdp_port <= 0 disables it (the update still deploys).
  std::string cdp_host = "127.0.0.1";
  int cdp_port = 0;
  std::string toast_text = "Deckback update installed \xE2\x80\x94 restart to apply.";
  int toast_ms = 8000;
  // Terminal deploy verdict (Done/Empty/Failed) with the portal's D-Bus error name and message.
  // Called on the updater's loop thread: hand the result to the UI thread, never draw from here.
  // When null, the updater shows its own "restart to apply" toast after a deploy.
  std::function<void(UpdateProgress, std::string error_name, std::string error_message)>
      on_terminal;
  // Each Running Progress: operation `op` (0-based) of `n_ops`, at `percent` (0–100, 100 once the
  // operation is done). Same thread rules as on_terminal.
  std::function<void(std::uint32_t op, std::uint32_t n_ops, std::uint32_t percent)> on_running{};
};

// Self-update through the Flatpak portal's UpdateMonitor (session bus via sd-bus; a no-op stub
// without libsystemd). Updates only this app's own ref from its `--user` remote, with no root.
// Game Mode cannot show the portal's consent dialog, so the updater records the consent itself
// (flatpak/updates=yes) unless the host explicitly says "no". A deployed commit binds on the next
// launch. A dropped bus or restarted portal is reconnected with backoff (busloop.hpp).
class Updater {
 public:
  static std::unique_ptr<Updater> create(UpdaterConfig cfg);
  virtual ~Updater() = default;

  // Create the portal update monitor and begin watching, on an owned event-loop thread. No-op if
  // the session-bus/portal connection is not live.
  virtual void start() = 0;
  // Signal + join the event-loop thread. Idempotent.
  virtual void stop() = 0;

  // Notify mode: consent to deploy the available update. Thread-safe; the deploy runs on the
  // updater's loop thread. No-op in the stub or with nothing pending.
  virtual void request_update() = 0;

  // True if a session-bus connection was established (does not probe the portal).
  virtual bool backend_live() const = 0;

  // `--selftest-update` (run inside the Flatpak): create a monitor and report whether the portal
  // answered. Returns 0 on success.
  virtual int selftest() = 0;

  // `--selftest-deploy`: drive one portal Update cycle to a terminal Progress on one connection
  // (UpdateMonitor signals are unicast to the creating connection). `seed` pre-grants consent
  // first; false uses the raw permission store.
  // Exit: 0 Done · 2 Empty · 3 Failed · 1 setup/timeout.
  virtual int selftest_deploy(bool seed) = 0;

  static bool backend_available();
};

}  // namespace deckback
