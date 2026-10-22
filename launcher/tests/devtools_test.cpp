#include "devtools.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "fake_cdp_server.hpp"
#include "harness.hpp"

using namespace deckback;
using deckback::testing::FakeServer;

namespace {

// ---- pure RFC 6455 frame codec ------------------------------------------------------------------

void expect_roundtrip(const std::string& payload, bool masked, uint32_t mask) {
  std::string wire = ws::encode_frame(ws::kText, payload, masked, mask);
  ws::DecodeResult dr = ws::decode_frame(wire);
  assert(dr.status == ws::DecodeResult::Ok);
  assert(dr.consumed == wire.size());
  assert(dr.frame.opcode == ws::kText);
  assert(dr.frame.fin);
  assert(dr.frame.payload == payload);
}

void test_codec_roundtrips() {
  // Boundary payload lengths exercise the 7-bit, 16-bit, and 64-bit length encodings.
  for (size_t n : {size_t(0), size_t(1), size_t(125), size_t(126), size_t(127), size_t(65535),
                   size_t(65536), size_t(100000)}) {
    std::string p(n, 'A');
    for (size_t i = 0; i < n; ++i) p[i] = static_cast<char>('a' + (i % 26));
    expect_roundtrip(p, /*masked=*/true, 0xDEADBEEF);
    expect_roundtrip(p, /*masked=*/false, 0);
  }
}

void test_codec_masking_actually_masks() {
  const std::string payload = "hello CDP";
  std::string masked = ws::encode_frame(ws::kText, payload, true, 0x11223344);
  std::string plain = ws::encode_frame(ws::kText, payload, false, 0);
  // Masked wire bytes must differ from the plaintext payload region, but decode back identically.
  assert(masked.find(payload) == std::string::npos);
  assert(plain.find(payload) != std::string::npos);
  assert(ws::decode_frame(masked).frame.payload == payload);
  // mask == 0 is a no-op XOR: payload bytes should appear verbatim even with the mask bit set.
  std::string zero_masked = ws::encode_frame(ws::kText, payload, true, 0);
  assert(zero_masked.find(payload) != std::string::npos);
  assert(ws::decode_frame(zero_masked).frame.payload == payload);
}

void test_codec_incomplete_is_needmore() {
  std::string wire = ws::encode_frame(ws::kText, std::string(300, 'x'), false, 0);
  for (size_t cut : {size_t(0), size_t(1), size_t(2), size_t(3), size_t(4), wire.size() - 1}) {
    ws::DecodeResult dr = ws::decode_frame(std::string_view(wire).substr(0, cut));
    assert(dr.status == ws::DecodeResult::NeedMore);
    assert(dr.consumed == 0);
  }
}

void test_codec_control_and_error() {
  ws::DecodeResult ping = ws::decode_frame(ws::encode_frame(ws::kPing, "hi", false, 0));
  assert(ping.status == ws::DecodeResult::Ok && ping.frame.opcode == ws::kPing);

  // A 64-bit length claiming > kMaxPayload must be rejected, not trusted into a huge allocation.
  std::string bomb;
  bomb.push_back(static_cast<char>(0x82));  // FIN + binary
  bomb.push_back(static_cast<char>(127));   // 64-bit length follows
  for (int i = 0; i < 8; ++i) bomb.push_back(static_cast<char>(0xFF));
  assert(ws::decode_frame(bomb).status == ws::DecodeResult::Error);
}

void test_codec_two_frames_in_buffer() {
  std::string a = ws::encode_frame(ws::kText, "one", false, 0);
  std::string b = ws::encode_frame(ws::kText, "two", false, 0);
  std::string both = a + b;
  ws::DecodeResult first = ws::decode_frame(both);
  assert(first.status == ws::DecodeResult::Ok && first.frame.payload == "one");
  ws::DecodeResult second = ws::decode_frame(std::string_view(both).substr(first.consumed));
  assert(second.status == ws::DecodeResult::Ok && second.frame.payload == "two");
}

void test_integration() {
  deckback::testing::FakeServer server;
  DevToolsClient client("127.0.0.1", server.port());

  auto b = client.eval_bool("document.querySelector('video') !== null");
  assert(b.has_value() && *b == true);
  assert(client.connected());  // session persists across calls

  auto n = client.eval_number("WANT_NUM currentTime");
  assert(n.has_value() && *n == 42.5);

  assert(client.eval_void("WANT_VOID play()"));

  // A CDP exceptionDetails reply is surfaced as failure, not a bogus value.
  auto exc = client.eval_bool("WANT_EXC broken");
  assert(!exc.has_value());

  // Events and stale-id replies interleaved before our response must be skipped, not mismatched.
  auto noisy = client.eval_bool("WANT_NOISE document.hidden");
  assert(noisy.has_value() && *noisy == true);

  // String reads are unescaped (\/ -> /).
  auto href = client.eval_string("WANT_STR document.location.href");
  assert(href.has_value() && *href == "https://www.youtube.com/tv#/");

  // A page string that merely contains the failure markers is a value, not a failure.
  auto trap = client.eval_string("WANT_TRAP document.title");
  assert(trap.has_value() && *trap == "{\"error\":1} exceptionDetails");

  // The UA override and navigate commands are accepted (no CDP error in the reply).
  assert(client.set_user_agent_override(
      "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/26.lts.0,gzip(gfe)"));
  assert(client.navigate("https://www.youtube.com/tv"));

  // Script install (with an embedded quote) and permission grant are accepted.
  assert(client.add_script_on_new_document("var q = \"av01\"; void q;"));
  assert(client.grant_permissions("https://www.youtube.com", "audioCapture"));
}

// ---- key/mouse dispatch -------------------------------------------------------------------------

void test_key_supported_set() {
  // Named navigation/media keys.
  assert(DevToolsClient::key_supported("ArrowUp"));
  assert(DevToolsClient::key_supported("Enter"));
  assert(DevToolsClient::key_supported("Backspace"));
  assert(DevToolsClient::key_supported("Delete"));
  assert(DevToolsClient::key_supported("MediaRewind"));
  assert(DevToolsClient::key_supported("MediaFastForward"));

  // Any single printable ASCII char: letters, digits, space, punctuation, shifted punctuation.
  assert(DevToolsClient::key_supported("c"));
  assert(DevToolsClient::key_supported("Z"));
  assert(DevToolsClient::key_supported("7"));
  assert(DevToolsClient::key_supported(" "));
  assert(DevToolsClient::key_supported("/"));
  assert(DevToolsClient::key_supported("?"));

  // Nothing else.
  assert(!DevToolsClient::key_supported(""));
  assert(!DevToolsClient::key_supported("ab"));
  assert(!DevToolsClient::key_supported("NoSuchKey"));
  assert(!DevToolsClient::key_supported("\n"));  // control chars are not printable
  assert(!DevToolsClient::key_supported("\x7f"));
}

// True iff `needle` occurs in some request whose method is `method`.
bool sent(const std::vector<std::string>& reqs, const std::string& method,
          const std::string& needle) {
  for (const std::string& r : reqs)
    if (r.find("\"method\":\"" + method + "\"") != std::string::npos &&
        r.find(needle) != std::string::npos)
      return true;
  return false;
}

int count_method(const std::vector<std::string>& reqs, const std::string& method) {
  int n = 0;
  for (const std::string& r : reqs)
    if (r.find("\"method\":\"" + method + "\"") != std::string::npos) ++n;
  return n;
}

void test_dispatch_key_wire_format() {
  deckback::testing::FakeServer server;
  DevToolsClient client("127.0.0.1", server.port());
  assert(client.eval_bool("true").has_value());  // force the session up
  server.take_requests();

  // A non-printable key: rawKeyDown + keyUp, no `text`, no `modifiers` when none were asked for.
  assert(client.dispatch_key("ArrowUp"));
  auto r = server.take_requests();
  assert(count_method(r, "Input.dispatchKeyEvent") == 2);
  assert(sent(r, "Input.dispatchKeyEvent", R"("type":"rawKeyDown")"));
  assert(sent(r, "Input.dispatchKeyEvent", R"("type":"keyUp")"));
  assert(sent(r, "Input.dispatchKeyEvent", R"("windowsVirtualKeyCode":38)"));
  assert(!sent(r, "Input.dispatchKeyEvent", R"("text")"));
  assert(!sent(r, "Input.dispatchKeyEvent", R"("modifiers")"));

  // Scan keys use the starboard/key.h VK codes (0xE3/0xE4).
  assert(client.dispatch_key("MediaRewind"));
  r = server.take_requests();
  assert(sent(r, "Input.dispatchKeyEvent", R"("key":"MediaRewind")"));
  assert(sent(r, "Input.dispatchKeyEvent", R"("windowsVirtualKeyCode":227)"));

  assert(client.dispatch_key("MediaFastForward"));
  r = server.take_requests();
  assert(sent(r, "Input.dispatchKeyEvent", R"("windowsVirtualKeyCode":228)"));

  assert(client.dispatch_key("Delete"));
  r = server.take_requests();
  assert(sent(r, "Input.dispatchKeyEvent", R"("windowsVirtualKeyCode":46)"));
}

void test_dispatch_printable_characters() {
  deckback::testing::FakeServer server;
  DevToolsClient client("127.0.0.1", server.port());
  assert(client.eval_bool("true").has_value());
  server.take_requests();

  // A printable key is keyDown (not rawKeyDown) carrying `text`, so it lands in a focused <input>.
  // `code` is the physical key.
  assert(client.dispatch_key("a"));
  auto r = server.take_requests();
  assert(sent(r, "Input.dispatchKeyEvent", R"("type":"keyDown")"));
  assert(!sent(r, "Input.dispatchKeyEvent", R"("type":"rawKeyDown")"));
  assert(sent(r, "Input.dispatchKeyEvent", R"("key":"a")"));
  assert(sent(r, "Input.dispatchKeyEvent", R"("code":"KeyA")"));
  assert(sent(r, "Input.dispatchKeyEvent", R"("windowsVirtualKeyCode":65)"));
  assert(sent(r, "Input.dispatchKeyEvent", R"("text":"a")"));

  // Uppercase keeps the physical code but reports the uppercase `key`/`text`.
  assert(client.dispatch_key("N", mod::kShift));
  r = server.take_requests();
  assert(sent(r, "Input.dispatchKeyEvent", R"("key":"N")"));
  assert(sent(r, "Input.dispatchKeyEvent", R"("code":"KeyN")"));
  assert(sent(r, "Input.dispatchKeyEvent", R"("windowsVirtualKeyCode":78)"));
  assert(sent(r, "Input.dispatchKeyEvent", R"("modifiers":8)"));  // Shift

  // Digits and space.
  assert(client.dispatch_key("7"));
  r = server.take_requests();
  assert(sent(r, "Input.dispatchKeyEvent", R"("code":"Digit7")"));
  assert(sent(r, "Input.dispatchKeyEvent", R"("windowsVirtualKeyCode":55)"));

  assert(client.dispatch_key(" "));
  r = server.take_requests();
  assert(sent(r, "Input.dispatchKeyEvent", R"("code":"Space")"));
  assert(sent(r, "Input.dispatchKeyEvent", R"("windowsVirtualKeyCode":32)"));

  // A shifted punctuation char carries the *physical* key it comes from ('?' lives on Slash).
  assert(client.dispatch_key("?", mod::kShift));
  r = server.take_requests();
  assert(sent(r, "Input.dispatchKeyEvent", R"("code":"Slash")"));
  assert(sent(r, "Input.dispatchKeyEvent", R"("windowsVirtualKeyCode":191)"));

  // A quote is JSON-escaped.
  assert(client.dispatch_key("\""));
  r = server.take_requests();
  assert(sent(r, "Input.dispatchKeyEvent", R"("key":"\"")"));
  assert(sent(r, "Input.dispatchKeyEvent", R"("text":"\"")"));

  // Modifiers combine as a bitfield.
  assert(client.dispatch_key("ArrowLeft", mod::kShift | mod::kCtrl));
  r = server.take_requests();
  assert(sent(r, "Input.dispatchKeyEvent", R"("modifiers":10)"));
}

void test_dispatch_key_refuses_unknown() {
  deckback::testing::FakeServer server;
  DevToolsClient client("127.0.0.1", server.port());
  assert(client.eval_bool("true").has_value());
  server.take_requests();

  // An unknown name dispatches nothing.
  assert(!client.dispatch_key("VolumeUp"));
  assert(!client.dispatch_key("ab"));
  assert(count_method(server.take_requests(), "Input.dispatchKeyEvent") == 0);
}

// ---- one socket, many clients -------------------------------------------------------------------

// Every DevToolsClient for one endpoint shares one CdpSession, i.e. one WebSocket.
void test_clients_to_one_endpoint_share_one_connection() {
  FakeServer srv;
  {
    DevToolsClient a("127.0.0.1", srv.port());
    DevToolsClient b("127.0.0.1", srv.port());
    DevToolsClient c("127.0.0.1", srv.port());
    // Connections are lazy: nothing is dialled until the first request.
    assert(srv.ws_upgrades() == 0);

    assert(a.eval_bool("WANT_TRUE") == true);
    assert(b.eval_bool("WANT_TRUE") == true);
    assert(c.eval_bool("WANT_TRUE") == true);

    assert(a.connected() && b.connected() && c.connected());
    // Three clients, one WebSocket, one target discovery.
    assert(srv.ws_upgrades() == 1);
    assert(srv.http_gets() == 1);
  }
}

// Sticky state (UA override, scripts) belongs to the session, not to the client that armed it.
void test_sticky_state_is_shared_and_survives_a_reconnect() {
  FakeServer srv;
  DevToolsClient nav("127.0.0.1", srv.port());
  DevToolsClient pad("127.0.0.1", srv.port());
  assert(nav.set_user_agent_override("TV-UA/1.0"));
  srv.take_requests();

  assert(pad.dispatch_key("Enter"));
  assert(srv.ws_upgrades() == 1);

  // The same sticky script added twice is remembered once.
  assert(nav.add_script_on_new_document("/*x*/1"));
  assert(nav.add_script_on_new_document("/*x*/1"));
  auto reqs = srv.take_requests();
  int installs = 0;
  for (const std::string& r : reqs)
    if (r.find("addScriptToEvaluateOnNewDocument") != std::string::npos) ++installs;
  assert(installs == 2);  // two explicit calls; a reconnect re-arms it once
}

// /json/list is sorted by last activity, so the client must pick the page target, not the first.
void test_attaches_to_the_page_not_the_first_target() {
  FakeServer srv;
  srv.set_list_decoy(true);
  DevToolsClient c("127.0.0.1", srv.port());
  assert(c.dispatch_key("Enter"));
  assert(srv.last_ws_path() == "/devtools/page/DECK");
}

// Several threads reconnecting at once after the socket dies: the process survives and replies
// keep arriving.
void test_concurrent_reconnects_do_not_crash() {
  FakeServer srv;
  srv.close_after_frames(3);
  std::atomic<int> delivered{0};
  std::vector<std::thread> threads;
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([&] {
      DevToolsClient c("127.0.0.1", srv.port());
      while (std::chrono::steady_clock::now() < until)
        if (c.eval_bool("WANT_TRUE") == true) delivered.fetch_add(1);
    });
  for (std::thread& t : threads) t.join();
  std::printf("  reconnects: %d, replies: %d\n", srv.ws_upgrades(), delivered.load());
  assert(srv.ws_upgrades() > 10);
  assert(delivered.load() > 10);
}

// Page.navigate replies at commit. One slower than the default request timeout still counts as
// sent and keeps the shared session.
void test_slow_navigation_is_not_a_transport_failure() {
  FakeServer srv;
  srv.set_navigate_delay_ms(3500);
  DevToolsClient c("127.0.0.1", srv.port());
  const DevToolsClient::NavStatus st = c.navigate_checked("https://www.youtube.com/tv");
  assert(st.sent);
  assert(st.error_text.empty());
  assert(srv.ws_upgrades() == 1);
}

// Different endpoints do not share: the registry is keyed by host:port.
void test_different_endpoints_do_not_share() {
  FakeServer a, b;
  DevToolsClient ca("127.0.0.1", a.port());
  DevToolsClient cb("127.0.0.1", b.port());
  assert(ca.eval_bool("WANT_TRUE") == true);
  assert(cb.eval_bool("WANT_TRUE") == true);
  assert(a.ws_upgrades() == 1);
  assert(b.ws_upgrades() == 1);
}

// A caller waiting on a reply does not block another thread from sending.
void test_requests_from_two_threads_do_not_serialize_on_a_lock() {
  FakeServer srv;
  DevToolsClient a("127.0.0.1", srv.port());
  DevToolsClient b("127.0.0.1", srv.port());
  assert(a.eval_bool("WANT_TRUE") == true);  // connect once, up front

  std::atomic<int> done{0};
  std::thread t1([&] {
    for (int i = 0; i < 50; ++i) assert(a.eval_bool("WANT_TRUE") == true);
    done.fetch_add(1);
  });
  std::thread t2([&] {
    for (int i = 0; i < 50; ++i) assert(b.dispatch_key("Enter"));
    done.fetch_add(1);
  });
  t1.join();
  t2.join();
  assert(done.load() == 2);
  assert(srv.ws_upgrades() == 1);  // 100 interleaved round trips, still one socket
}

void test_dispatch_mouse() {
  deckback::testing::FakeServer server;
  DevToolsClient client("127.0.0.1", server.port());
  assert(client.eval_bool("true").has_value());
  server.take_requests();

  // CDP mouse coords are CSS pixels; no letterbox transform is applied.
  assert(client.mouse_click(640.5, 360));
  auto r = server.take_requests();
  assert(count_method(r, "Input.dispatchMouseEvent") == 2);
  assert(sent(r, "Input.dispatchMouseEvent", R"("type":"mousePressed")"));
  assert(sent(r, "Input.dispatchMouseEvent", R"("type":"mouseReleased")"));
  assert(sent(r, "Input.dispatchMouseEvent", R"("x":640.5)"));
  assert(sent(r, "Input.dispatchMouseEvent", R"("y":360)"));
  assert(sent(r, "Input.dispatchMouseEvent", R"("button":"left")"));

  // press and release can be sent separately.
  assert(client.mouse_press(10, 20));
  r = server.take_requests();
  assert(count_method(r, "Input.dispatchMouseEvent") == 1);
  assert(sent(r, "Input.dispatchMouseEvent", R"("type":"mousePressed")"));
  assert(sent(r, "Input.dispatchMouseEvent", R"("buttons":1)"));

  assert(client.mouse_release(10, 20));
  r = server.take_requests();
  assert(count_method(r, "Input.dispatchMouseEvent") == 1);
  assert(sent(r, "Input.dispatchMouseEvent", R"("type":"mouseReleased")"));
  assert(sent(r, "Input.dispatchMouseEvent", R"("buttons":0)"));
}

void test_unreachable_server() {
  // Nothing listening on this port: every call fails cleanly and we never claim connected.
  DevToolsClient client("127.0.0.1", 8);  // port 8 is unassigned/unlikely to listen
  assert(!client.eval_bool("true").has_value());
  assert(!client.eval_number("1").has_value());
  assert(!client.eval_void("void 0"));
  assert(!client.connected());
}

}  // namespace

int index_of(const std::vector<std::string>& reqs, const std::string& method) {
  for (size_t i = 0; i < reqs.size(); ++i)
    if (reqs[i].find("\"method\":\"" + method + "\"") != std::string::npos)
      return static_cast<int>(i);
  return -1;
}

// A fresh socket is unusable until sticky state is re-armed, so another thread's navigate lands
// after the UA override. The fake delays the re-arm's first reply to widen the window.
void test_rearm_finishes_before_another_caller_navigates() {
  FakeServer srv;
  DevToolsClient nav("127.0.0.1", srv.port());
  DevToolsClient other("127.0.0.1", srv.port());
  assert(nav.set_user_agent_override("TV-UA/1.0"));
  srv.close_after_frames(static_cast<int>(srv.take_requests().size()));  // drop the next frame
  assert(!other.eval_bool("WANT_TRUE").has_value());                     // ...and the socket
  srv.close_after_frames(0);
  srv.set_method_delay_ms("Page.enable", 300);  // the re-arm's first request
  srv.take_requests();

  std::thread rearm([&] { other.eval_bool("WANT_TRUE"); });  // reconnects, then re-arms
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  std::thread go([&] { nav.navigate("https://www.youtube.com/tv"); });
  rearm.join();
  go.join();

  const std::vector<std::string> reqs = srv.take_requests();
  const int ua = index_of(reqs, "Network.setUserAgentOverride");
  const int navigate = index_of(reqs, "Page.navigate");
  assert(ua >= 0 && navigate >= 0);
  assert(ua < navigate);
}

// Page.enable is sent once per connection, and Network.enable never (it would stream an event per
// media segment; the UA override does not need it).
void test_enable_commands_are_not_repeated() {
  FakeServer srv;
  DevToolsClient c("127.0.0.1", srv.port());
  assert(c.set_user_agent_override("TV-UA/1.0"));
  assert(c.navigate("about:blank"));
  assert(c.navigate("about:blank"));
  assert(c.reload());
  assert(c.add_script_on_new_document("/*y*/1"));
  std::vector<std::string> reqs = srv.take_requests();
  assert(count_method(reqs, "Page.enable") == 1);
  assert(count_method(reqs, "Network.enable") == 0);
  assert(count_method(reqs, "Network.setUserAgentOverride") >= 1);

  // A new connection is armed again, once, before anything else runs on it.
  srv.close_after_frames(static_cast<int>(reqs.size()));  // drop the next frame and the socket
  assert(!c.eval_bool("WANT_TRUE").has_value());
  srv.close_after_frames(0);
  assert(c.navigate("about:blank"));
  reqs = srv.take_requests();
  assert(count_method(reqs, "Page.enable") == 1);
  assert(count_method(reqs, "Page.addScriptToEvaluateOnNewDocument") == 1);
  assert(index_of(reqs, "Network.setUserAgentOverride") < index_of(reqs, "Page.navigate"));
}

DECKBACK_TEST_MAIN(devtools) {
  test_codec_roundtrips();
  test_codec_masking_actually_masks();
  test_codec_incomplete_is_needmore();
  test_codec_control_and_error();
  test_codec_two_frames_in_buffer();
  test_integration();
  test_key_supported_set();
  test_dispatch_key_wire_format();
  test_dispatch_printable_characters();
  test_dispatch_key_refuses_unknown();
  test_clients_to_one_endpoint_share_one_connection();
  test_sticky_state_is_shared_and_survives_a_reconnect();
  test_different_endpoints_do_not_share();
  test_attaches_to_the_page_not_the_first_target();
  test_concurrent_reconnects_do_not_crash();
  test_slow_navigation_is_not_a_transport_failure();
  test_requests_from_two_threads_do_not_serialize_on_a_lock();
  test_dispatch_mouse();
  test_unreachable_server();
  test_rearm_finishes_before_another_caller_navigates();
  test_enable_commands_are_not_repeated();
  std::puts("devtools_test: ok");
  return 0;
}
