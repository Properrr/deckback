#pragma once

#include <condition_variable>
#include <mutex>
#include <string_view>
#include <thread>

namespace deckback {

// Pure, X-free helper (so it is unit-testable): does a window's WM_CLASS mark it as one of OUR
// content_shell windows? gamescope reports WM_CLASS as null-separated "instance\0class\0", e.g.
// "content_shell\0Content_shell\0" or "chromium-content_shell\0...". Case-insensitive substring
// match on "content_shell" covers all of ours without matching Steam's own windows.
bool focus_class_is_ours(std::string_view wm_class);

// Option B of `disable_touch`: hold gamescope's GLOBAL touch click mode at 0 (hover) — a finger
// moves the cursor but generates no click — but ONLY while our window is focused. gamescope 3.16
// exposes no per-window override (the mode is the Steam-managed root atom STEAM_TOUCH_CLICK_MODE),
// so asserting it unconditionally would also kill touch in the Steam overlay/QAM. We therefore poll
// the focused window and assert hover only when it is ours; when focus is elsewhere we leave
// Steam's value alone.
//
// Best-effort and defensive: if built without libxcb, or X is unreachable (no DISPLAY), or the atom
// is absent, it logs once and does nothing — the page-level pointer swallow (Option A) still makes
// taps inert. Runs on its own thread; stop() joins.
class TouchModeGuard {
 public:
  explicit TouchModeGuard(int poll_ms = 750);
  ~TouchModeGuard();

  void start();
  void stop();

 private:
  void loop();
  bool wait_or_stop(int ms);

  int poll_ms_;
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  bool started_ = false;
};

}  // namespace deckback
