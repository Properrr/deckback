#include "touchmode.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

#include "harness.hpp"
#include "scripts.hpp"  // the pointer-swallow script now lives in the ScriptLibrary registry

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
  // Steam's own windows must NOT trigger hover mode, or we'd kill touch in the overlay/QAM.
  assert(!focus_class_is_ours(wm_class("steam", "Steam")));
  assert(!focus_class_is_ours(wm_class("steamwebhelper", "steamwebhelper")));
  assert(!focus_class_is_ours(""));
  assert(!focus_class_is_ours("gamescope"));

  // --- Assert every tick while focused. REGRESSION FIXTURE, not a unit of arithmetic. ---
  //
  // The function is two lines, so what this really pins is the SHAPE of the decision: focus is the
  // whole input, and the atom's current value is deliberately NOT one. The version that read the
  // atom and wrote only on a mismatch killed touch for entire sessions and sent a day of
  // investigation at the engine binary (touch-gestures.md §7.0.2) — on-Deck, :0 read 1 while :1
  // read 4 at the same instant and behaved as 1. Our copy is a stale mirror of a global mode Steam
  // sets from its own display, and Steam re-asserts 1 every time a game takes focus, so a guard
  // that trusts the read, or that writes only once, always loses.
  //
  // If someone "optimises" this by taking the current value as a parameter again, this
  // static_assert should stop the build. That is what it is for.
  static_assert(
      std::is_invocable_r_v<bool, decltype(should_write_mode), bool>,
      "should_write_mode must decide on focus ALONE — re-adding the atom's current value as an "
      "input is the regression this fixture exists to block (touch-gestures.md §7.0.2)");

  // Focused: assert, on every tick, INCLUDING when our mirror already reads the mode we want. X11
  // emits a PropertyNotify on every write regardless of value, and that event is the entire
  // payload — it is what makes gamescope re-point touch at our window.
  assert(should_write_mode(true));

  // Not focused: silent. The one thing the old guard got right, and it must stay: asserting a
  // GLOBAL setting while Steam owns the screen would fight the overlay for it.
  assert(!should_write_mode(false));

  // --- Option A embedded script: present, and covers the events that navigate. ---
  std::string js(deckback::ScriptLibrary::instance().body("no_pointer"));
  assert(!js.empty());
  // The kill is only real if it stops propagation before Leanback's handlers run.
  assert(js.find("stopImmediatePropagation") != std::string::npos);
  assert(js.find("capture") != std::string::npos);
  // The events a tap/drag produces under gamescope (mouse, not touch) must all be listed, plus the
  // touch events for completeness.
  for (const char* ev :
       {"pointerdown", "mousedown", "mouseup", "click", "mousemove", "touchstart", "contextmenu"}) {
    assert(js.find(ev) != std::string::npos);
  }
  // ...and it hides the cursor (gamescope draws our X cursor, so cursor:none makes it disappear).
  assert(js.find("cursor") != std::string::npos);
  assert(js.find("none") != std::string::npos);
  assert(js.find("adoptedStyleSheets") != std::string::npos);  // the CSP-safe injection path

  std::puts("touchmode_test: ok");
  return 0;
}
