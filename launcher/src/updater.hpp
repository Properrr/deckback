#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace deckback {

// Shared, thread-safe hand-off from the updater thread (which detects an available update) to the
// input thread (which draws the one-time card + amber pill). Mirrors the relaxed-atomic
// OverlayState/LayerState pattern. Keyed on the portal's remote ostree commit — the precise dedup
// key (version labels are cosmetic to the portal, durable/self-update.md).
class UpdateState {
 public:
  // Publish "a newer commit R is available" (updater thread). Dismissal ("ignore this version") is
  // keyed to the commit via the on-disk dot marker (decide_notification), so a newer commit
  // naturally re-arms the pill — this state carries only availability + the commit.
  void set_available(bool available, std::string commit);
  bool available() const { return available_.load(std::memory_order_relaxed); }
  std::string commit() const;  // the short remote commit, mutex-guarded

 private:
  std::atomic<bool> available_{false};
  mutable std::mutex m_;
  std::string commit_;
};

// Flatpak portal `Progress.status` codes (data/org.freedesktop.portal.Flatpak.xml):
// 0 Running, 1 Empty (nothing to do), 2 Done, 3 Failed. Pure decoder, always compiled.
enum class UpdateProgress { Running, Empty, Done, Failed, Unknown };
UpdateProgress decode_progress_status(std::uint32_t status);

// flatpak-portal gates a self-update deploy on the permission store (table "flatpak", id
// "updates"): "yes" deploys silently, anything else routes through an Access-portal consent dialog
// that Game Mode cannot show (durable/self-update.md ★ T1 RESULT). Mirrors flatpak's
// get_update_permission(): only the first element counts, "yes"/"ask" match exactly, any other
// value is No, no entry is Unset. Pure decoder, always compiled.
enum class UpdatePermission { Unset, Ask, Yes, No };
UpdatePermission decode_update_permission(const std::vector<std::string>& perms);
const char* update_permission_name(UpdatePermission p);

// App id from /.flatpak-info text (`[Application]` section, `name=` key); empty when not sandboxed.
std::string parse_flatpak_app_id(const std::string& text);

// Session-bus reconnect backoff (durable/dbus-reconnect.md). When the updater's session-bus
// connection drops mid-session, or flatpak-portal restarts and orphans the monitor, the loop
// re-establishes instead of going inert until relaunch. Pure + always compiled so the schedule is
// L0-tested where the sd-bus loop compiles out.
//
// reconnect_delay_ms: exponential backoff from 1s, doubling, capped at 60s; the shift is clamped so
// any attempt index is defined (0→1000, 1→2000, … 5→32000, ≥6→60000).
std::uint64_t reconnect_delay_ms(unsigned attempt);
// reconnect_should_give_up: after this many consecutive failed (re)connect attempts (~6–7 min of
// backoff) the watcher goes inert until the next launch — the connect-once floor, just deferred.
bool reconnect_should_give_up(unsigned consecutive_failures);

// Self-update through the Flatpak portal (`org.freedesktop.portal.Flatpak`'s UpdateMonitor).
//
// A sandboxed app cannot run `flatpak update`; instead it asks the portal to update the app on its
// behalf. Only this app's own ref is updated, from its own `--user` `deckback` remote — no root, no
// polkit prompt, and nothing else the user installed is touched. Because Game Mode has no
// Access-portal backend for flatpak-portal's consent dialog, an enabled updater records the consent
// itself (permission store, flatpak/updates=yes) before watching; an explicit host-side "no" is
// respected. The new ostree commit is deployed without swapping the running deployment, so playback
// is not interrupted; it binds on the next launch, when a "restart to apply" toast is shown if a
// CDP port is available.
//
// Backed by libsystemd sd-bus on the session bus; a no-op stub without libsystemd.
struct UpdaterConfig {
  // The Updater is constructed only in `notify`/`auto` (never `off`), so being constructed already
  // means "enabled". `auto_deploy` distinguishes the two: true deploys on detection (auto), false
  // only publishes availability and waits for request_update() (notify).
  bool auto_deploy = false;
  // Where the updater publishes "an update is available" for the UI threads. Non-owning; must
  // outlive the Updater. Null in `auto`-only builds/tests that don't wire the notify UI.
  UpdateState* state = nullptr;
  // Only for the "restart to apply" toast; cdp_port <= 0 disables the toast (the update still
  // deploys). Host/port match the engine's --remote-debugging-port (loopback).
  std::string cdp_host = "127.0.0.1";
  int cdp_port = 0;
  std::string toast_text = "Deckback update installed \xE2\x80\x94 restart to apply.";
  int toast_ms = 8000;
};

class Updater {
 public:
  static std::unique_ptr<Updater> create(UpdaterConfig cfg);
  virtual ~Updater() = default;

  // Create the portal update monitor and begin watching, on an owned event-loop thread. No-op if
  // the session-bus/portal connection is not live.
  virtual void start() = 0;
  // Signal + join the event-loop thread. Idempotent.
  virtual void stop() = 0;

  // Consent to deploy the currently-available update (notify mode): thread-safe, called from the UI
  // thread when the user chooses "Update now". The deploy happens on the updater's own loop thread
  // (never reentrantly), exactly as an `auto` deploy would. No-op in the stub / if nothing pending.
  virtual void request_update() = 0;

  // True if a live session-bus connection was established (the portal itself is probed by
  // selftest).
  virtual bool backend_live() const = 0;

  // Live self-test (needs the portal on the session bus, i.e. run inside the Flatpak): create a
  // monitor and report whether the portal answered. Returns 0 on success. `--selftest-update`.
  virtual int selftest() = 0;

  // Diagnostic (`--selftest-deploy`): drive one portal Update cycle to a terminal Progress and
  // report it, on one persistent connection (UpdateMonitor signals are unicast to the creating
  // connection, so a transient dbus-send/gdbus can't observe them). `seed` false reflects the raw
  // permission store (reproduces the Game-Mode "No portal support found" blocker); `seed` true
  // pre-grants consent first. Exit: 0 Done · 2 Empty · 3 Failed · 1 setup/timeout.
  virtual int selftest_deploy(bool seed) = 0;

  // Diagnostic (`--selftest-watch <secs>`): run the real updater loop for up to `secs` seconds,
  // then stop cleanly. Nothing is deployed (notify mode with no consent) — the point is to keep the
  // loop alive so an external actor can drop the session bus (case A) or restart flatpak-portal
  // (case B) and the reconnect handling logs its progress. This is what the containerized `just sim
  // reconnect` drive exercises off-hardware (durable/dbus-reconnect.md, durable/test-sim.md).
  // Interruptible by SIGINT/SIGTERM for a clean early stop. Exit: 0 (ran and stopped) · 1 (no live
  // session bus).
  virtual int selftest_watch(int secs) = 0;

  static bool backend_available();
};

}  // namespace deckback
