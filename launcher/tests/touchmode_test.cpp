#include "touchmode.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

#include "harness.hpp"
#include "scripts.hpp"

using deckback::focus_class_is_ours;
using deckback::should_write_mode;

// WM_CLASS is two NUL-separated strings; build one the way xcb hands it to us.
static std::string wm_class(const char* instance, const char* klass) {
  std::string s(instance);
  s.push_back('\0');
  s += klass;
  s.push_back('\0');
  return s;
}

DECKBACK_TEST_MAIN(touchmode) {
  // --- Option B focus matcher: only OUR content_shell windows count as focused. ---
  assert(focus_class_is_ours(wm_class("content_shell", "Content_shell")));
  assert(focus_class_is_ours(wm_class("chromium-content_shell", "chromium-content_shell")));
  assert(focus_class_is_ours("Content_shell"));  // case-insensitive
  // Steam's own windows do not count, so touch keeps working in the overlay/QAM.
  assert(!focus_class_is_ours(wm_class("steam", "Steam")));
  assert(!focus_class_is_ours(wm_class("steamwebhelper", "steamwebhelper")));
  assert(!focus_class_is_ours(""));
  assert(!focus_class_is_ours("gamescope"));

  // --- Write every tick while focused. ---
  // The decision depends on focus alone: the atom value we can read is a stale mirror of a global
  // mode Steam re-asserts, so it must not be an input.
  static_assert(
      std::is_invocable_r_v<bool, decltype(should_write_mode), bool>,
      "should_write_mode must decide on focus ALONE — re-adding the atom's current value as an "
      "input is the regression this fixture exists to block (touch-gestures.md §7.0.2)");

  // Focused: write, even if the value is unchanged; the PropertyNotify each write emits is what
  // makes gamescope re-point touch at our window.
  assert(should_write_mode(true));

  // Not focused: never write the global setting while Steam owns the screen.
  assert(!should_write_mode(false));

  // --- Option A embedded script: present, and covers the events that navigate. ---
  std::string js(deckback::ScriptLibrary::instance().body("no_pointer"));
  assert(!js.empty());
  // Capture-phase stopImmediatePropagation, so Leanback's handlers never run.
  assert(js.find("stopImmediatePropagation") != std::string::npos);
  assert(js.find("capture") != std::string::npos);
  // The mouse events a tap produces under gamescope, plus touch events.
  for (const char* ev :
       {"pointerdown", "mousedown", "mouseup", "click", "mousemove", "touchstart", "contextmenu"}) {
    assert(js.find(ev) != std::string::npos);
  }
  // It hides the cursor (gamescope draws our X cursor).
  assert(js.find("cursor") != std::string::npos);
  assert(js.find("none") != std::string::npos);
  assert(js.find("adoptedStyleSheets") != std::string::npos);  // the CSP-safe injection path

  std::puts("touchmode_test: ok");
  return 0;
}
