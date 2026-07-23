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
#include "scripts.hpp"

using namespace deckback;

namespace {

bool has(const std::string& s, const std::string& needle) {
  return s.find(needle) != std::string::npos;
}

// ---- toast_js (now config/scripts/toast.js rendered with text/ms params) ------------------------

void test_toast_js_shape() {
  const std::string js = toast_js("Touchscreen locked", 2000);
  assert(has(js, "__deckback_toast"));
  // Appended to documentElement, not body: Leanback replaces body content on navigation.
  assert(has(js, "document.documentElement.appendChild"));
  // Reused by id, so a second toast replaces the first instead of stacking a second <div> forever.
  assert(has(js, "getElementById"));
  // Styling via CSSOM, NOT setAttribute('style',…): youtube.com/tv's CSP style-src has no
  // 'unsafe-inline', so the attribute path is dropped and the toast renders unstyled
  // (self-update.md).
  assert(!has(js, "setAttribute('style'"));
  assert(!has(js, "setAttribute(\"style\""));
  // It must never eat a tap meant for the page underneath.
  assert(has(js, "setProperty('pointer-events', 'none')"));
  // The timeout handle is cleared before being re-armed, or a rapid lock/unlock leaves the older
  // timer to hide the newer toast.
  assert(has(js, "clearTimeout(window.__deckbackToastT)"));
  // text/ms now arrive as JSON params on the invocation, not interpolated into the body.
  assert(has(js, "\"ms\":2000"));
  assert(has(js, "\"text\":\"Touchscreen locked\""));
}

void test_toast_js_escapes_quotes_and_backslashes() {
  // The text is now a JSON string param, escaped once by ScriptParams. An unescaped quote would
  // close the object literal early — the same syntax-error risk, centralised.
  assert(has(toast_js("say \"hi\"", 100), R"("text":"say \"hi\"")"));
  assert(has(toast_js("a\\b", 100), R"("text":"a\\b")"));
  // A lone trailing backslash would otherwise escape the string's closing quote.
  assert(has(toast_js("dir\\", 100), R"("text":"dir\\")"));
}

void test_toast_js_escapes_newlines() {
  // The lock toast is two lines ("Touchscreen locked\nHold l3+r3 to unlock"). A raw newline inside
  // the JSON string is a syntax error, so this is the shipped path, not a hypothetical one.
  const std::string js = toast_js("one\ntwo", 100);
  assert(has(js, R"("text":"one\ntwo")"));
  assert(!has(js, "one\ntwo"));  // no raw newline survived into the param
  // pre-wrap (via CSSOM) keeps the escaped \n a line break like `pre` did, and additionally wraps a
  // long single line instead of clipping it off both edges.
  assert(has(js, "setProperty('white-space', 'pre-wrap')"));
}

void test_toast_js_clamps_negative_duration() {
  // setTimeout(fn, -1) fires immediately: the toast would flash and vanish. The clamp now lives in
  // the script (p.ms < 0 ? 0 : p.ms), so the param carries -1 and the body defends against it.
  const std::string js = toast_js("x", -1);
  assert(has(js, "\"ms\":-1"));
  assert(has(js, "p.ms < 0 ? 0 : p.ms"));
}

void test_toast_hide_js() {
  const std::string js = toast_hide_js();
  assert(has(js, "__deckback_toast"));
  assert(has(js, "clearTimeout"));  // else the pending fade-out fires over the next toast
  assert(has(js, "opacity"));
}

// ---- Trusted Types (folded into the HTML-injecting scripts) -------------------------------------

void test_trusted_types_folded_into_html_scripts() {
  // js_trusted_html is gone; the policy youtube.com/tv's CSP requires now lives INSIDE the two
  // scripts that assign innerHTML. Without it, the assignment silently throws on the real page (the
  // card rendered in tests but nothing on the Deck until this was added — input-ux §17).
  for (const char* name : {"overlay", "error_page"}) {
    const std::string b(ScriptLibrary::instance().body(name));
    assert(has(b, "trustedTypes"));
    assert(has(b, "createPolicy"));
    assert(has(b, "createHTML"));
    // Memoised on window, or every re-inject after a navigation throws "policy already exists".
    assert(has(b, "__dbTTP"));
    // ...and it degrades to the raw string in a try/catch (about:blank has no Trusted Types).
    assert(has(b, "catch"));
  }
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
  test_trusted_types_folded_into_html_scripts();

  test_show_toast_evaluates_on_the_page();
  test_show_toast_survives_a_dead_engine();

  test_make_rumble_effect();
  test_haptic_attach_fails_cleanly_on_a_non_device();

  std::puts("overlay_test: all assertions passed");
  return 0;
}
