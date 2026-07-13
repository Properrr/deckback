#include "touchmode.hpp"

#include <string>

#include "log.hpp"
#include "util.hpp"

namespace deckback {

bool focus_class_is_ours(std::string_view wm_class) {
  // Lowercase copy (WM_CLASS may embed NULs between instance and class — harmless for a substring
  // search) and look for our engine's class token.
  return ascii_lower(wm_class).find("content_shell") != std::string::npos;
}

}  // namespace deckback

#if defined(DECKBACK_HAVE_XCB)

#include <xcb/xcb.h>

#include <cstdint>
#include <cstdlib>

namespace deckback {
namespace {

// Typed zero-constants: XCB_NONE / XCB_ATOM_NONE are an untyped macro and an enum, so using them in
// `?:` or comparisons against xcb_window_t / xcb_atom_t trips -Werror=extra (enum vs non-enum).
constexpr xcb_window_t kNoWindow = XCB_NONE;
constexpr xcb_atom_t kNoAtom = XCB_ATOM_NONE;

constexpr uint32_t kHoverMode =
    0;  // gamescope --default-touch-mode: 0=hover (cursor moves, no click)

// Read a 32-bit CARDINAL property, or -1 if absent/short.
long read_cardinal(xcb_connection_t* c, xcb_window_t w, xcb_atom_t prop) {
  xcb_get_property_reply_t* r =
      xcb_get_property_reply(c, xcb_get_property(c, 0, w, prop, XCB_ATOM_CARDINAL, 0, 1), nullptr);
  long out = -1;
  if (r && xcb_get_property_value_length(r) >= 4)
    out = *static_cast<uint32_t*>(xcb_get_property_value(r));
  free(r);
  return out;
}

// WM_CLASS of a window as a raw string (may contain an embedded NUL), empty if unset.
std::string window_class(xcb_connection_t* c, xcb_window_t w) {
  xcb_get_property_reply_t* r = xcb_get_property_reply(
      c, xcb_get_property(c, 0, w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 128), nullptr);
  std::string out;
  if (r) {
    int len = xcb_get_property_value_length(r);
    if (len > 0)
      out.assign(static_cast<char*>(xcb_get_property_value(r)), static_cast<size_t>(len));
  }
  free(r);
  return out;
}

xcb_window_t parent_of(xcb_connection_t* c, xcb_window_t w) {
  xcb_query_tree_reply_t* r = xcb_query_tree_reply(c, xcb_query_tree(c, w), nullptr);
  xcb_window_t parent = r ? r->parent : kNoWindow;
  free(r);
  return parent;
}

// Is the currently input-focused window (or one of its ancestors — gamescope may reparent, and
// focus can land on a child that carries no WM_CLASS) one of ours?
bool our_window_is_focused(xcb_connection_t* c, xcb_window_t root) {
  xcb_get_input_focus_reply_t* fr = xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr);
  xcb_window_t w = fr ? fr->focus : kNoWindow;
  free(fr);
  for (int i = 0; i < 8 && w != kNoWindow && w != root; ++i) {
    if (focus_class_is_ours(window_class(c, w))) return true;
    w = parent_of(c, w);
  }
  return false;
}

}  // namespace

void TouchModeGuard::loop() {
  int screen_num = 0;
  xcb_connection_t* c = xcb_connect(nullptr, &screen_num);
  if (!c || xcb_connection_has_error(c)) {
    warn(
        "touch: gamescope hover guard: cannot reach X (no DISPLAY?); relying on page swallow only");
    if (c) xcb_disconnect(c);
    return;
  }
  const xcb_setup_t* setup = xcb_get_setup(c);
  xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
  for (int i = 0; i < screen_num && it.rem; ++i) xcb_screen_next(&it);
  xcb_window_t root = it.data ? it.data->root : kNoWindow;

  const char* kName = "STEAM_TOUCH_CLICK_MODE";
  xcb_intern_atom_reply_t* ar = xcb_intern_atom_reply(
      c, xcb_intern_atom(c, 0, static_cast<uint16_t>(std::char_traits<char>::length(kName)), kName),
      nullptr);
  xcb_atom_t mode_atom = ar ? ar->atom : kNoAtom;
  free(ar);
  if (root == kNoWindow || mode_atom == kNoAtom) {
    warn("touch: gamescope hover guard: no root/atom; relying on page swallow only");
    xcb_disconnect(c);
    return;
  }

  bool announced = false;
  for (;;) {
    if (our_window_is_focused(c, root) && read_cardinal(c, root, mode_atom) != kHoverMode) {
      uint32_t v = kHoverMode;
      xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, mode_atom, XCB_ATOM_CARDINAL, 32, 1, &v);
      xcb_flush(c);
      if (!announced) {
        info(
            "touch: gamescope touch mode held at hover while focused (taps move cursor, no click)");
        announced = true;
      }
    }
    if (worker_.wait_or_stop(poll_ms_)) break;
  }
  xcb_disconnect(c);
}

}  // namespace deckback

#else  // no libxcb

namespace deckback {
void TouchModeGuard::loop() {
  warn(
      "touch: built without libxcb — gamescope hover guard unavailable; page pointer swallow "
      "(Option A) still makes taps inert");
}
}  // namespace deckback

#endif

namespace deckback {

TouchModeGuard::TouchModeGuard(int poll_ms) : poll_ms_(poll_ms > 0 ? poll_ms : 750) {}

TouchModeGuard::~TouchModeGuard() { stop(); }

void TouchModeGuard::start() {
  worker_.start([this] { loop(); });
}

void TouchModeGuard::stop() { worker_.stop(); }

}  // namespace deckback
