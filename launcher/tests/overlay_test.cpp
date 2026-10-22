// The CDP-injected toast and the force-feedback effect. A JS error in the toast is ignored at
// runtime, so escaping bugs are only visible here. Whether the pad accepts the FF upload needs a
// Deck.
#include "overlay.hpp"

#include <cassert>
#include <cstdio>
#include <string>

#include "fake_cdp_server.hpp"
#include "haptic.hpp"
#include "harness.hpp"
#include "scripts.hpp"

using namespace deckback;

namespace {

bool has(const std::string& s, const std::string& needle) {
  return s.find(needle) != std::string::npos;
}

// ---- toast_js (config/scripts/toast.js rendered with text/ms params) ----------------------------

void test_toast_js_shape() {
  const std::string js = toast_js("Touchscreen locked", 2000);
  assert(has(js, "__deckback_toast"));
  // Appended to documentElement, not body: Leanback replaces body content on navigation.
  assert(has(js, "document.documentElement.appendChild"));
  // Reused by id, so a second toast replaces the first.
  assert(has(js, "getElementById"));
  // Styled via CSSOM: the page's CSP drops inline style attributes.
  assert(!has(js, "setAttribute('style'"));
  assert(!has(js, "setAttribute(\"style\""));
  assert(has(js, "setProperty('pointer-events', 'none')"));
  // The previous timer is cleared, so it cannot hide a newer toast.
  assert(has(js, "clearTimeout(window.__deckbackToastT)"));
  // text/ms arrive as JSON params on the invocation.
  assert(has(js, "\"ms\":2000"));
  assert(has(js, "\"text\":\"Touchscreen locked\""));
}

void test_toast_js_escapes_quotes_and_backslashes() {
  // The text is a JSON string param, escaped once by ScriptParams.
  assert(has(toast_js("say \"hi\"", 100), R"("text":"say \"hi\"")"));
  assert(has(toast_js("a\\b", 100), R"("text":"a\\b")"));
  // A trailing backslash must not escape the closing quote.
  assert(has(toast_js("dir\\", 100), R"("text":"dir\\")"));
}

void test_toast_js_escapes_newlines() {
  const std::string js = toast_js("one\ntwo", 100);
  assert(has(js, R"("text":"one\ntwo")"));
  assert(!has(js, "one\ntwo"));  // no raw newline survived into the param
  // pre-wrap keeps \n as a line break and wraps long lines.
  assert(has(js, "setProperty('white-space', 'pre-wrap')"));
}

void test_toast_js_clamps_negative_duration() {
  // The param carries -1 and the script clamps it.
  const std::string js = toast_js("x", -1);
  assert(has(js, "\"ms\":-1"));
  assert(has(js, "p.ms < 0 ? 0 : p.ms"));
}

void test_toast_hide_js() {
  const std::string js = toast_hide_js();
  assert(has(js, "__deckback_toast"));
  assert(has(js, "clearTimeout"));
  assert(has(js, "opacity"));
}

// ---- Trusted Types ------------------------------------------------------------------------------

void test_trusted_types_folded_into_html_scripts() {
  // The scripts that assign innerHTML create the Trusted Types policy the page's CSP requires.
  for (const char* name : {"overlay", "error_page"}) {
    const std::string b(ScriptLibrary::instance().body(name));
    assert(has(b, "trustedTypes"));
    assert(has(b, "createPolicy"));
    assert(has(b, "createHTML"));
    // Memoised on window, so a re-inject does not recreate it.
    assert(has(b, "__dbTTP"));
    // Falls back to the raw string where Trusted Types is absent (about:blank).
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
  // With CDP down, show_toast and hide_toast return without throwing or blocking.
  DevToolsClient c("127.0.0.1", 8);  // nothing listening
  show_toast(c, "Captions on", 2000);
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
  // No trigger button, so the kernel never replays it on its own.
  assert(e.trigger.button == 0);
  assert(e.trigger.interval == 0);

  // An existing id is preserved, so repeated pulses reuse one effect slot.
  const ff_effect reused = make_rumble_effect(7, 0x1000, 0x2000, 90);
  assert(reused.id == 7);
  assert(reused.u.rumble.strong_magnitude == 0x1000);
  assert(reused.u.rumble.weak_magnitude == 0x2000);
  assert(reused.replay.length == 90);
}

void test_haptic_attach_fails_cleanly_on_a_non_device() {
  // A node with no FF, or one that cannot be opened, just leaves rumble off.
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

DECKBACK_TEST_MAIN(overlay) {
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
