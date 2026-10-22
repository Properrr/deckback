#pragma once

#include <cstdint>
#include <string_view>

#include "worker.hpp"

namespace deckback {

// Does a window's WM_CLASS mark it as one of our content_shell windows? gamescope reports WM_CLASS
// as null-separated "instance\0class\0", e.g. "content_shell\0Content_shell\0" or
// "chromium-content_shell\0...". Case-insensitive substring match on "content_shell".
bool focus_class_is_ours(std::string_view wm_class);

// Should the guard write the mode atom this tick? Whenever our window is focused — unconditionally,
// WITHOUT reading the value already there:
//  1. The value we can read is a stale mirror. gamescope runs one Xwayland per display, each with
//     its own copy of the atom, but enforces one global mode; Steam writes on :0, we read on :1.
//  2. Steam re-asserts its own mode on every focus change, so a guard that writes once loses.
// X11 emits a PropertyNotify on every change_property call even when the value is unchanged, and
// that event is what makes gamescope re-point touch at our window.
bool should_write_mode(bool focused);

// Holds gamescope's global touch click mode (the Steam-managed root atom STEAM_TOUCH_CLICK_MODE) at
// `mode`, but only while our window is focused; otherwise Steam's value is left alone. X input
// focus does not move when the Steam QAM opens over us, so "focused" means roughly "our app is the
// running game".
//
// Best-effort: without libxcb, without an X display, or without the atom it logs once and does
// nothing. Runs on its own thread; stop() joins.
//
// gamescope modes: 0 = hover (motion only, no click), 1-3 = synthesized mouse click, 4 =
// passthrough (the only mode that delivers real wl_touch to the page).
enum class TouchMode : uint32_t {
  kHover = 0,        // cursor moves, no click — the disable_touch lock
  kPassthrough = 4,  // real wl_touch, NO pointer emulation — what the gesture router reads
};

class TouchModeGuard {
 public:
  explicit TouchModeGuard(int poll_ms = 750, TouchMode mode = TouchMode::kHover);
  ~TouchModeGuard();

  void start();
  void stop();

 private:
  void loop();

  // [[maybe_unused]]: the no-xcb build of loop() never reads these (clang's
  // -Wunused-private-field).
  [[maybe_unused]] int poll_ms_;
  [[maybe_unused]] TouchMode mode_;
  WorkerThread worker_;
};

}  // namespace deckback
