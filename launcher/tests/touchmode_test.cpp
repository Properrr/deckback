#include "touchmode.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "harness.hpp"
#include "scripts.hpp"  // the pointer-swallow script now lives in the ScriptLibrary registry

using deckback::focus_class_is_ours;

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
