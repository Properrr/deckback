#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "about.hpp"  // AboutInfo
#include "devtools.hpp"
#include "onboarding.hpp"  // OverlayContext + controls_overlay_rows
#include "scripts.hpp"     // ScriptParams

namespace deckback {

class CaptionSettings;

// In-app OSD Settings menu (findings osd-menu-plan.md). Focus/tab/scroll state lives in
// config/scripts/osd.js; this owns open/close, the top-right Settings button, playback gating, and
// the C++<->JS command bridge. Invariant: capture <=> paint — exec() returning "gone" clears open_.

// ---- pure helpers (unit-tested in osdmenu_test.cpp) ----

std::string osd_status_line(std::string_view local_version, std::string_view available_version,
                            bool has_update);

std::vector<std::pair<std::string, std::string>> osd_update_buttons(bool has_update);

struct OsdVerdict {
  // Apply = a settings change to persist WITHOUT closing the menu (the Captions sub-tab); Action =
  // a one-shot that closes (the Updates buttons).
  enum class Kind { Consumed, Close, Action, Apply, Gone } kind = Kind::Consumed;
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
  // Captions sub-tab: read at open (osd_model_json) and mutated on each edit (apply_action). Not
  // owned; null hides the sub-tab. Same object the input layer's View toggle reads, so an OSD edit
  // takes effect on the next press with no restart.
  CaptionSettings* captions = nullptr;
  std::function<void()> on_update_confirm;
  std::function<void()> on_update_ignore;
};

class OsdMenuController {
 public:
  explicit OsdMenuController(OsdMenuConfig cfg);

  OsdMenuController(const OsdMenuController&) = delete;
  OsdMenuController& operator=(const OsdMenuController&) = delete;

  // Modal-capture flag, read by the input thread every event.
  bool open() const { return open_.load(std::memory_order_acquire); }

  // Deliberate open (Menu button, off playback). False if the engine is unreachable.
  bool open_menu();
  void close_menu();

  // Forward one command; act on the verdict; return the raw verdict.
  std::string exec(std::string_view cmd);

  // Per input-tick with the raw watch signal (LayerState::video_up).
  void tick(bool on_watch);

  // Thread-safe: feed the Updates tab + button badge. `status` is supplied by the update-policy
  // owner rather than inferred from `has_update`: false means either "no update" or "not checked",
  // and the OSD must never turn that ambiguity into a false "latest" claim.
  void set_update_model(bool has_update, std::string_view status, std::string_view notes);
  bool update_available() const;

  // Navigator thread, on a full page reload.
  void on_page_reloaded();

 private:
  std::string eval_op(DevToolsClient& client, const ScriptParams& params);
  bool inject_open(DevToolsClient& client);
  void reconcile_open();    // capture <=> paint: release capture if the page reloaded the menu away
  void reconcile_button();  // redraw after a same-URL reload that Navigator cannot observe
  void draw_button();
  void hide_button();

  OsdMenuConfig cfg_;
  DevToolsClient client_;

  std::atomic<bool> open_{false};
  std::atomic<bool> reloaded_{false};

  bool button_shown_ = false;
  bool badge_dirty_ = true;

  std::chrono::steady_clock::time_point last_reconcile_{};
  std::chrono::steady_clock::time_point last_button_reconcile_{};

  mutable std::mutex model_mu_;
  bool has_update_ = false;
  std::string status_;
  std::string notes_;
};

}  // namespace deckback
