#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "about.hpp"  // AboutInfo
#include "devtools.hpp"
#include "onboarding.hpp"   // OverlayContext + controls_overlay_rows
#include "pageoverlay.hpp"  // PageOverlay, Throttle
#include "scripts.hpp"      // ScriptParams

namespace deckback {

class CaptionSettings;

// In-app OSD Settings menu. Focus/tab/scroll state lives in config/scripts/osd.js; this owns
// open/close, the top-right Settings button, playback gating, and the C++<->JS command bridge.
// Invariant: capture <=> paint — exec() returning "gone" clears open_; an unanswered command
// (kOsdUnreachable) does not, because it proves nothing about the paint.

// The DOM id config/scripts/osd_button.js assigns; also used by PageOverlay's presence probe.
inline constexpr const char* kOsdButtonElementId = "__deckback_settings_btn";

// ---- pure helpers (unit-tested in osdmenu_test.cpp) ----

std::string osd_status_line(std::string_view local_version, std::string_view available_version,
                            bool has_update);

// Updates-tab actions. `has_update` offers the deploy pair; `can_check` offers a manual check;
// `can_quit` offers Close now once an update is staged (it applies on the next launch).
std::vector<std::pair<std::string, std::string>> osd_update_buttons(bool has_update, bool can_check,
                                                                    bool can_quit);

// What exec() returns when the engine did not answer. Not a string osd.js ever produces.
inline constexpr std::string_view kOsdUnreachable = "unreachable";

struct OsdVerdict {
  // Apply = a settings change to persist WITHOUT closing the menu (Captions); Action = a one-shot
  // that closes (Updates buttons); Hold = Y started the hold-to-exit, which acts
  // once the caller's hold deadline elapses. Unreachable = no answer: capture is kept and the
  // reconcile tick decides.
  enum class Kind { Consumed, Close, Action, Apply, Hold, Gone, Unreachable } kind = Kind::Consumed;
  std::string action;
};
OsdVerdict parse_verdict(std::string_view verdict);

// ---- controller ----

struct OsdMenuConfig {
  std::string cdp_host = "127.0.0.1";
  int cdp_port = 0;
  std::string local_version;
  OverlayContext overlay;  // live keymap -> Keys rows; const after ctor
  AboutInfo about;         // About tab content (from the AppStream metainfo)
  // Captions sub-tab: read at open and mutated on each edit. Not owned; null hides the sub-tab.
  // Shared with the input layer, so an edit takes effect on the next press.
  CaptionSettings* captions = nullptr;
  std::function<void()> on_update_confirm;
  std::function<void()> on_update_ignore;
  std::function<void()> on_update_check;
  // Close now (a staged update). Null leaves the button inert; wire it like on_exit.
  std::function<void()> on_update_quit;
  // Fired when the Y hold-to-exit completes. Null leaves it inert.
  std::function<void()> on_exit;
  // How long Y must be held to exit; also drives the fill animation.
  int exit_hold_ms = 600;
  bool exit_enabled = true;
  // While *app_ready is false, tick() leaves the page alone (no Settings button yet).
  const std::atomic<bool>* app_ready = nullptr;
};

class OsdMenuController {
 public:
  explicit OsdMenuController(OsdMenuConfig cfg);

  OsdMenuController(const OsdMenuController&) = delete;
  OsdMenuController& operator=(const OsdMenuController&) = delete;

  // Modal-capture flag, read by the input thread every event.
  bool open() const { return open_.load(std::memory_order_acquire); }

  // Deliberate open (Menu button), on the watch screen too. False if the engine is unreachable.
  bool open_menu();
  void close_menu();

  // Forward one command; act on the verdict; return the raw verdict.
  std::string exec(std::string_view cmd);

  // How long Y must be held to exit, for the caller that owns the deadline.
  int exit_hold_ms() const { return cfg_.exit_hold_ms; }

  // The hold completed: run the exit. Safe to call with no handler configured.
  void fire_exit();

  // Per input-tick with the raw watch signal (LayerState::video_up).
  void tick(bool on_watch);

  // Thread-safe: feed the Updates tab + button badge. `status` is supplied, not inferred from
  // `has_update`, whose false means either "no update" or "not checked".
  void set_update_model(bool has_update, bool can_check, bool can_quit, std::string_view status,
                        std::string_view notes);
  bool update_available() const;

  // Navigator thread, on a full page reload.
  void on_page_reloaded();

 private:
  // The osd.js reply, or nullopt when the engine did not answer.
  std::optional<std::string> eval_op(DevToolsClient& client, const ScriptParams& params);
  bool inject_open(DevToolsClient& client);
  void reconcile_open();  // capture <=> paint: release capture if the page reloaded the menu away
  void draw_button();
  void hide_button();

  OsdMenuConfig cfg_;
  DevToolsClient client_;

  // Patch the Updates panel of an already-open menu (input thread).
  void push_update_model();

  std::atomic<bool> open_{false};
  std::atomic<bool> reloaded_{false};

  // The top-right Settings button. PageOverlay re-checks presence, since a same-URL reload wipes
  // the DOM without any signal.
  PageOverlay button_;
  bool badge_dirty_ = true;
  // A model change not yet pushed to the open menu (otherwise a snapshot taken at open).
  std::atomic<bool> model_dirty_{false};

  // Paces the menu liveness probe, which asks osd.js for its state rather than checking presence.
  Throttle menu_probe_{kOverlayReconcileMs};

  mutable std::mutex model_mu_;
  bool has_update_ = false;
  bool can_check_ = false;
  bool can_quit_ = false;
  std::string status_;
  std::string notes_;
};

}  // namespace deckback
