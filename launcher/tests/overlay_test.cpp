// Touch-lock feedback (findings input-ux §4): the CDP-injected toast and the force-feedback effect.
//
// The toast exists because the lock is otherwise unobservable — a locked touchscreen and a hung
// browser look identical. So the failure this file guards against is a toast that *silently does
// not appear*: `Runtime.evaluate` on malformed JS raises a JS exception the launcher deliberately
// ignores (a failed toast must not break the lock), which means an escaping bug produces no error
// anywhere. Only a test can catch it.
//
// What is NOT covered here, and cannot be: whether EVIOCGRAB actually starves gamescope of touch
// events, and whether the pad accepts our FF upload. Both need the Deck (.internal/TEST-PLAN.md
// §2).
#include "overlay.hpp"

#include <cassert>
#include <cstdio>
#include <string>

#include "fake_cdp_server.hpp"
#include "haptic.hpp"

using namespace deckback;

namespace {

bool has(const std::string& s, const std::string& needle) {
  return s.find(needle) != std::string::npos;
}

// ---- toast_js -----------------------------------------------------------------------------------

void test_toast_js_shape() {
  const std::string js = toast_js("Touchscreen locked", 2000);
  assert(has(js, "__deckback_toast"));
  // Appended to documentElement, not body: Leanback replaces body content on navigation.
  assert(has(js, "document.documentElement.appendChild"));
  // Reused by id, so a second toast replaces the first instead of stacking a second <div> forever.
  assert(has(js, "getElementById"));
  // It must never eat a tap meant for the page underneath.
  assert(has(js, "pointer-events:none"));
  // The timeout handle is cleared before being re-armed, or a rapid lock/unlock leaves the older
  // timer to hide the newer toast.
  assert(has(js, "clearTimeout(window.__deckbackToastT)"));
  assert(has(js, "2000)"));
  assert(has(js, "Touchscreen locked"));
}

void test_toast_js_escapes_quotes_and_backslashes() {
  // The text is interpolated into a double-quoted JS string literal. An unescaped quote closes it
  // early and the whole statement is a syntax error — which we would never see, because
  // show_toast() ignores the result by design.
  const std::string js = toast_js("say \"hi\"", 100);
  assert(has(js, "d.textContent=\"say \\\"hi\\\"\";"));

  const std::string b = toast_js("a\\b", 100);
  assert(has(b, "d.textContent=\"a\\\\b\";"));
  // A lone trailing backslash would otherwise escape the literal's closing quote.
  const std::string t = toast_js("dir\\", 100);
  assert(has(t, "d.textContent=\"dir\\\\\";"));
}

void test_toast_js_escapes_newlines() {
  // The lock toast is two lines ("Touchscreen locked\nHold l3+r3 to unlock"). A raw newline inside
  // a JS string literal is a syntax error, so this is the shipped path, not a hypothetical one.
  const std::string js = toast_js("one\ntwo", 100);
  assert(has(js, "d.textContent=\"one\\ntwo\";"));
  assert(!has(js, "\"one\ntwo\""));  // no raw newline survived
  // white-space:pre, or the escaped \n renders as a space.
  assert(has(js, "white-space:pre"));
}

void test_toast_js_clamps_negative_duration() {
  // setTimeout(fn, -1) fires immediately: the toast would flash and vanish. Clamp rather than
  // trust.
  const std::string js = toast_js("x", -1);
  assert(has(js, ",0)"));
  assert(!has(js, "-1)"));
}

void test_toast_hide_js() {
  const std::string js = toast_hide_js();
  assert(has(js, "__deckback_toast"));
  assert(has(js, "clearTimeout"));  // else the pending fade-out fires over the next toast
  assert(has(js, "opacity"));
}

// ---- js_trusted_html ----------------------------------------------------------------------------

void test_trusted_html_wraps_and_falls_back() {
  const std::string js = js_trusted_html("\"<h2>hi</h2>\"");
  // The raw HTML must still be in there -- we wrap it, we don't drop it.
  assert(has(js, "<h2>hi</h2>"));
  // It goes through a Trusted Types policy when one is available...
  assert(has(js, "trustedTypes"));
  assert(has(js, "createPolicy"));
  assert(has(js, "createHTML"));
  // ...and returns the raw string when it is not (about:blank) or when policy creation throws.
  assert(has(js, "if(!T)return"));
  assert(has(js, "catch"));
  // The policy is memoised, or every re-inject after a navigation throws "policy already exists".
  assert(has(js, "__dbTTP"));
}

void test_trusted_html_is_a_single_expression() {
  // It is dropped straight into `d.innerHTML=<here>;`, so it must be one parenthesised expression,
  // not a statement. A leading '(' and an IIFE call are the cheap structural proxy for that.
  const std::string js = js_trusted_html("\"x\"");
  assert(!js.empty());
  assert(js.front() == '(');
  assert(js.back() == ')');
}

// ---- show_toast over CDP ------------------------------------------------------------------------

void test_show_toast_evaluates_on_the_page() {
  testing::FakeServer server;
  DevToolsClient c("127.0.0.1", server.port());
  server.take_requests();

  show_toast(c, "Touchscreen locked", 2000);
  const auto r = server.take_requests();
  bool saw = false;
  for (const std::string& req : r)
    if (has(req, "\"method\":\"Runtime.evaluate\"") && has(req, "__deckback_toast")) saw = true;
  assert(saw);
}

void test_show_toast_survives_a_dead_engine() {
  // The contract: feedback is secondary to the action it reports. If CDP is down, the touch lock
  // must still engage — so show_toast must return, not throw, not block.
  DevToolsClient c("127.0.0.1", 8);  // nothing listening
  show_toast(c, "Touchscreen locked", 2000);
  hide_toast(c);
}

// ---- make_rumble_effect -------------------------------------------------------------------------

void test_make_rumble_effect() {
  const ff_effect e = make_rumble_effect(-1, 0xC000, 0x4000, 180);
  assert(e.type == FF_RUMBLE);
  assert(e.id == -1);  // -1 = "kernel, allocate me a slot"
  assert(e.replay.length == 180);
  assert(e.replay.delay == 0);
  assert(e.u.rumble.strong_magnitude == 0xC000);
  assert(e.u.rumble.weak_magnitude == 0x4000);
  // A non-zero trigger would make the kernel replay the effect on a button press of its own.
  assert(e.trigger.button == 0);
  assert(e.trigger.interval == 0);

  // An existing id is preserved, so repeated pulses reuse one slot rather than exhausting the
  // device's finite effect table over a long session.
  const ff_effect reused = make_rumble_effect(7, 0x1000, 0x2000, 90);
  assert(reused.id == 7);
  assert(reused.u.rumble.strong_magnitude == 0x1000);
  assert(reused.u.rumble.weak_magnitude == 0x2000);
  assert(reused.replay.length == 90);
}

void test_haptic_attach_fails_cleanly_on_a_non_device() {
  // A pad with no FF, or a node we cannot open O_RDWR, is ordinary — not an error, just no rumble.
  Haptic h;
  assert(!h.attach("/dev/null"));  // opens, but advertises no EV_FF
  assert(!h.attached());
  assert(!h.attach("/nonexistent/deckback/event999"));
  assert(!h.attached());
  h.rumble(0x8000, 0x8000, 100);  // inert, must not crash
  h.detach();                     // idempotent
  h.detach();
}

}  // namespace

int main() {
  test_toast_js_shape();
  test_toast_js_escapes_quotes_and_backslashes();
  test_toast_js_escapes_newlines();
  test_toast_js_clamps_negative_duration();
  test_toast_hide_js();
  test_trusted_html_wraps_and_falls_back();
  test_trusted_html_is_a_single_expression();

  test_show_toast_evaluates_on_the_page();
  test_show_toast_survives_a_dead_engine();

  test_make_rumble_effect();
  test_haptic_attach_fails_cleanly_on_a_non_device();

  std::puts("overlay_test: all assertions passed");
  return 0;
}
