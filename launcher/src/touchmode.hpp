#pragma once

#include <cstdint>
#include <string_view>

#include "worker.hpp"

namespace deckback {

// Pure, X-free helper (so it is unit-testable): does a window's WM_CLASS mark it as one of OUR
// content_shell windows? gamescope reports WM_CLASS as null-separated "instance\0class\0", e.g.
// "content_shell\0Content_shell\0" or "chromium-content_shell\0...". Case-insensitive substring
// match on "content_shell" covers all of ours without matching Steam's own windows.
bool focus_class_is_ours(std::string_view wm_class);

// Pure, X-free helper (so it is unit-testable): should the guard WRITE the mode atom this tick?
//
// The answer is "whenever our window is focused" — unconditionally, every tick, WITHOUT consulting
// the value already there. That looks wasteful and is not; the version that read the atom first and
// wrote "only when it is wrong" is the bug that killed touch for entire sessions and cost a day of
// blaming the engine binary (touch-gestures.md §7.0.2). Two measured facts kill the read:
//
//  1. The value we can read is a STALE MIRROR. gamescope runs one Xwayland per display, each with
//     its own copy of this atom, but enforces ONE global mode. Steam writes its value on :0; we
//     read ours on :1. Measured on-Deck with touch demonstrably broken: :0 = 1 while :1 = 4, and
//     the effective behaviour was 1. Our copy can read "correct" while the real mode is anything.
//  2. We are CONTESTING the atom, not initialising it. Steam sets mode 1 (click emulation) whenever
//     a game is focused, and re-asserts on every focus change — measured: GAMESCOPE_FOCUSED_APP
//     769 (Steam) -> mode 4, our appid -> mode 1, repeatedly. A guard that writes once always
//     loses.
//
// So the guard must continuously assert, and the write must happen even when our mirror already
// says 4: X11 emits a PropertyNotify on every change_property call whether or not the value
// differs, and that event is the whole payload — it is what makes gamescope re-point touch at our
// window.
//
// Verified safe on-Deck at a 750 ms cadence: 59 swipe sequences recognised with writes landing
// mid-gesture and none disrupted, and the Steam QAM stayed touch-usable throughout (Steam itself
// wants mode 4 while its overlay is focused, so we agree with it exactly when it matters).
bool should_write_mode(bool focused);

// Option B of `disable_touch`: hold gamescope's GLOBAL touch click mode at 0 (hover) — a finger
// moves the cursor but generates no click — but ONLY while our window is focused. gamescope 3.16
// exposes no per-window override (the mode is the Steam-managed root atom STEAM_TOUCH_CLICK_MODE),
// so it is not ours to hold unconditionally. We poll the focused window and assert only when it is
// ours; when focus is elsewhere we leave Steam's value alone.
//
// Caveat worth knowing before trusting that gate: X input focus does NOT move when the Steam QAM
// opens over us (measured — 95 consecutive samples, all "chromium-content_shell"), so "focused"
// here means roughly "our app is the running game", not "the user is looking at us". Asserting
// through a QAM session turned out to be harmless in practice — Steam wants passthrough for its own
// overlay anyway — but if a future policy needs to genuinely stand down while Steam owns the
// screen, GAMESCOPE_FOCUSED_APP on :0's root is the signal that actually tracks it.
//
// Best-effort and defensive: if built without libxcb, or X is unreachable (no DISPLAY), or the atom
// is absent, it logs once and does nothing — the page-level pointer swallow (Option A) still makes
// taps inert. Runs on its own thread; stop() joins.
//
// The held mode is a parameter because the gesture router needs the OPPOSITE of the lock. Mode 4
// (passthrough) is the only mode that delivers real wl_touch — gamescope's wlserver.cpp calls
// wlr_seat_touch_notify_down with a per-finger id, while modes 1-3 merely synthesize a mouse button
// and mode 0 emits nothing but motion. Verified on-Deck 2026-07-31: at mode 4 a finger produces
// touchstart/touchmove/touchend in Blink; at mode 0 it produces only mousemove. So `disable_touch`
// holds 0 and `touch_gestures` holds 4, through one guard.
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
