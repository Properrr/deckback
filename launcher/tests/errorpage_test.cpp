// The kiosk failure state (findings input-ux §7). Covers the net::-error classifier, the retry
// backoff, the injected page's JS, and — the part that matters — that a *failed* navigation is
// recognised as failed.
//
// The trap this file exists to catch: CDP reports a failed navigation as a **successful** command
// carrying an `errorText` field, and the failed document keeps `location.href` set to the URL we
// asked for. So every obvious way to ask "did we land on the app?" answers yes while the user is
// staring at Chromium's desktop error interstitial — a page with no control a controller can focus,
// in an app with no address bar and no keyboard. The bug is silent, and it is the exact state a
// broken Wi-Fi connection puts us in.
#include "errorpage.hpp"

#include <cassert>
#include <cstdio>
#include <string>

#include "fake_cdp_server.hpp"
#include "harness.hpp"

using namespace deckback;

namespace {

bool has(const std::string& s, const std::string& needle) {
  return s.find(needle) != std::string::npos;
}

// ---- navigate_checked ---------------------------------------------------------------------------

void test_navigate_reports_success() {
  testing::FakeServer server;
  DevToolsClient c("127.0.0.1", server.port());
  const auto st = c.navigate_checked("https://www.youtube.com/tv");
  assert(st.sent);
  assert(st.error_text.empty());
  assert(st.ok());
}

void test_navigate_surfaces_error_text() {
  // The whole point: the CDP command SUCCEEDS. `sent` is true, there is no CDP error, and the only
  // evidence of failure is a field in the result.
  testing::FakeServer server;
  server.set_nav_error("net::ERR_NAME_NOT_RESOLVED");
  DevToolsClient c("127.0.0.1", server.port());

  const auto st = c.navigate_checked("https://www.youtube.com/tv");
  assert(st.sent);                                        // the command went through...
  assert(st.error_text == "net::ERR_NAME_NOT_RESOLVED");  // ...and it still failed
  assert(!st.ok());
}

void test_navigate_unreachable_engine_is_not_a_page_error() {
  // Transport failure and navigation failure demand different responses: one retries the socket,
  // the other draws a page. Conflating them would have us try to render an error page onto an
  // engine that is not there.
  DevToolsClient c("127.0.0.1", 8);  // nothing listening
  const auto st = c.navigate_checked("https://www.youtube.com/tv");
  assert(!st.sent);
  assert(st.error_text.empty());
  assert(!st.ok());
}

// ---- classify_net_error -------------------------------------------------------------------------

void test_classify_known_errors() {
  assert(has(classify_net_error("net::ERR_INTERNET_DISCONNECTED"), "isn't connected"));
  assert(has(classify_net_error("net::ERR_NAME_NOT_RESOLVED"), "DNS"));
  assert(has(classify_net_error("net::ERR_CERT_DATE_INVALID"), "clock"));
  assert(has(classify_net_error("net::ERR_PROXY_CONNECTION_FAILED"), "proxy"));
  assert(has(classify_net_error("net::ERR_CONNECTION_TIMED_OUT"), "timed out"));
  assert(has(classify_net_error("net::ERR_CONNECTION_REFUSED"), "refused"));
}

void test_classify_refuses_to_guess() {
  // An unrecognised error gets an honest, vague hint — never a confident wrong one. Telling someone
  // to check their Wi-Fi when the certificate expired sends them off to fix the wrong thing.
  const std::string generic = classify_net_error("net::ERR_SOMETHING_WE_HAVE_NEVER_SEEN");
  assert(!generic.empty());
  assert(!has(generic, "Wi-Fi"));
  assert(!has(generic, "DNS"));
  assert(!has(generic, "clock"));
  assert(!has(generic, "proxy"));

  // Distinct errors that we *do* know must not collapse onto the generic hint.
  assert(classify_net_error("net::ERR_NAME_NOT_RESOLVED") != generic);
  assert(classify_net_error("net::ERR_INTERNET_DISCONNECTED") != generic);
  // Different causes, different advice.
  assert(classify_net_error("net::ERR_NAME_NOT_RESOLVED") !=
         classify_net_error("net::ERR_INTERNET_DISCONNECTED"));

  assert(classify_net_error("").empty());  // no error, no hint
}

// ---- retry_backoff_ms ---------------------------------------------------------------------------

void test_backoff_doubles_and_caps() {
  assert(retry_backoff_ms(0, 2000, 30000) == 2000);
  assert(retry_backoff_ms(1, 2000, 30000) == 4000);
  assert(retry_backoff_ms(2, 2000, 30000) == 8000);
  assert(retry_backoff_ms(3, 2000, 30000) == 16000);
  assert(retry_backoff_ms(4, 2000, 30000) == 30000);  // capped, not 32000
  assert(retry_backoff_ms(99, 2000, 30000) == 30000);
}

void test_backoff_never_spins() {
  // app.json is remotely hot-swappable, so these are untrusted inputs. Every one of them must yield
  // a delay a poll loop can survive: a zero delay against a dead network is a busy loop that also
  // hammers the engine with a navigation per iteration.
  assert(retry_backoff_ms(0, 0, 0) >= 250);
  assert(retry_backoff_ms(5, 0, 0) >= 250);
  assert(retry_backoff_ms(0, -1000, -1) >= 250);
  assert(retry_backoff_ms(-7, 2000, 30000) == 2000);  // a negative attempt is the first attempt
  // max below min: the cap wins nothing, but the result must still be sane.
  assert(retry_backoff_ms(3, 5000, 1000) == 5000);

  // The doubling must saturate, never overflow. This needs a *small min and a huge max*, so the
  // loop actually runs and `delay *= 2` walks up to the signed limit. (min == max never enters the
  // loop, which is why an earlier version of this test proved nothing.) Signed overflow is UB and
  // the plausible outcome is a negative delay: an immediate retry, re-navigating every iteration.
  const long huge = (1L << 62) + 12345;
  for (int attempt : {40, 61, 62, 63, 64, 1000}) {
    const long d = retry_backoff_ms(attempt, 250, huge);
    assert(d > 0);      // never wrapped negative
    assert(d <= huge);  // never exceeded the cap
    assert(d >= 250);   // never below the floor
  }
  // And it really does reach the cap rather than stalling partway.
  assert(retry_backoff_ms(1000, 250, huge) == huge);
  assert(retry_backoff_ms(40, 1L << 62, 1L << 62) == (1L << 62));
}

// ---- error_page_js ------------------------------------------------------------------------------

ErrorPageInfo sample() {
  ErrorPageInfo i;
  i.title = "Can't reach YouTube";
  i.detail = "net::ERR_NAME_NOT_RESOLVED";
  i.hint = "Check your Wi-Fi.";
  i.url = "https://www.youtube.com/tv";
  return i;
}

void test_error_page_js_contains_the_user_facing_parts() {
  const std::string js = error_page_js(sample());
  assert(has(js, "Can't reach YouTube"));
  assert(has(js, "Check your Wi-Fi."));
  assert(has(js, "net::ERR_NAME_NOT_RESOLVED"));  // small print, for whoever files the bug
  assert(has(js, "https://www.youtube.com/tv"));

  // A focusable, focused button — on a controller there is no cursor, so an unfocused button is an
  // unreachable one, which is the very failure this page replaces.
  assert(has(js, "__deckback_retry"));
  assert(has(js, "b.focus()"));
  assert(has(js, "outline"));  // a visible focus ring

  // Enter and Space retry. Escape must NOT be bound: the only thing it could do is quit, and
  // quitting the app because the Wi-Fi blinked is not a kindness.
  assert(has(js, "e.key==='Enter'"));
  assert(has(js, "e.key===' '"));
  assert(!has(js, "Escape"));

  // The flag starts cleared, or a stale retry from a previous outage fires instantly.
  assert(has(js, "window.__deckbackRetry=false;"));
  // The marker the launcher polls to see whether its page is still the loaded document.
  assert(has(js, "__deckback_error"));
}

void test_error_page_js_escapes_hostile_text() {
  // `error_hint` and `error_title` come from app.json, which is designed to be hot-swapped remotely
  // to fix a broken Leanback (R1). A quote in that text would close the JS string literal and the
  // page would never render — and nothing would report it, because show_error_page's failure mode
  // is a blank screen, not an error.
  ErrorPageInfo i = sample();
  i.title = "say \"hi\"";
  i.hint = "back\\slash";
  i.detail = "line1\nline2";
  const std::string js = error_page_js(i);
  assert(has(js, "say \\\"hi\\\""));
  assert(has(js, "back\\\\slash"));
  assert(has(js, "line1\\nline2"));
  assert(!has(js, "line1\nline2"));  // no raw newline inside the literal
}

void test_retry_flag_expression_consumes() {
  // Read-and-clear in one round trip. A read that does not clear would turn one keypress into an
  // infinite retry loop, each iteration re-navigating.
  const std::string e = kRetryFlagExpr;
  assert(has(e, "window.__deckbackRetry"));
  assert(has(e, "=false"));
  assert(has(e, "return r"));
}

// ---- show_error_page / take_retry_request over CDP ----------------------------------------------

void test_show_error_page_navigates_to_blank_first() {
  // Not a bundled file:// page and not a data: URL — the first is one more sandbox path to get
  // wrong, the second is a restricted top-frame navigation. about:blank always commits, even when
  // the network is the thing that failed.
  testing::FakeServer server;
  server.set_nav_error("net::ERR_INTERNET_DISCONNECTED");  // everything else fails
  DevToolsClient c("127.0.0.1", server.port());
  server.take_requests();

  assert(show_error_page(c, sample()));
  const auto reqs = server.take_requests();
  bool blank = false, injected = false;
  for (const std::string& r : reqs) {
    if (has(r, "Page.navigate") && has(r, "about:blank")) blank = true;
    if (has(r, "Runtime.evaluate") && has(r, "__deckback_error")) injected = true;
  }
  assert(blank && injected);
  assert(server.error_page_up());
}

void test_take_retry_request_is_one_shot() {
  testing::FakeServer server;
  DevToolsClient c("127.0.0.1", server.port());

  assert(!take_retry_request(c));  // nothing pressed
  server.press_retry();
  assert(take_retry_request(c));   // consumed...
  assert(!take_retry_request(c));  // ...exactly once
}

void test_take_retry_request_on_dead_engine_is_false() {
  // nullopt must not read as "the user pressed retry". A dead socket answering "yes" would send the
  // navigator into a re-navigation loop against an engine that is not there.
  DevToolsClient c("127.0.0.1", 8);
  assert(!take_retry_request(c));
}

}  // namespace

DECKBACK_TEST_MAIN(errorpage) {
  test_navigate_reports_success();
  test_navigate_surfaces_error_text();
  test_navigate_unreachable_engine_is_not_a_page_error();

  test_classify_known_errors();
  test_classify_refuses_to_guess();

  test_backoff_doubles_and_caps();
  test_backoff_never_spins();

  test_error_page_js_contains_the_user_facing_parts();
  test_error_page_js_escapes_hostile_text();
  test_retry_flag_expression_consumes();

  test_show_error_page_navigates_to_blank_first();
  test_take_retry_request_is_one_shot();
  test_take_retry_request_on_dead_engine_is_false();

  std::puts("errorpage_test: all assertions passed");
  return 0;
}
