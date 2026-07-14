// Phase 5 voice search (findings input-ux §13). Covers the pure hold-to-talk state machine,
// duck-mode and point parsing, the mic-button probe JS, and VoiceController's CDP behaviour against
// a fake soft-mic button.
//
// The behaviour under test that matters most is the NEGATIVE one: when Leanback exposes no soft-mic
// button — the expected outcome if our Cobalt UA hides it (§13.2, the unverified V0 spike) — the
// launcher must click nothing, duck nothing, and say so. A dead button is worse than a missing one.
#include "voice.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "fake_cdp_server.hpp"

using namespace deckback;

namespace {

int count_method(const std::vector<std::string>& reqs, const std::string& method) {
  int n = 0;
  for (const std::string& r : reqs)
    if (r.find("\"method\":\"" + method + "\"") != std::string::npos) ++n;
  return n;
}

bool sent(const std::vector<std::string>& reqs, const std::string& needle) {
  for (const std::string& r : reqs)
    if (r.find(needle) != std::string::npos) return true;
  return false;
}

// ---- duck modes ---------------------------------------------------------------------------------

void test_parse_duck_mode() {
  assert(parse_duck_mode("none") == DuckMode::None);
  assert(parse_duck_mode("mute") == DuckMode::Mute);
  assert(parse_duck_mode("pause") == DuckMode::Pause);
  // Unrecognised must be nullopt so startup can warn, not silently pick a behaviour.
  assert(!parse_duck_mode("").has_value());
  assert(!parse_duck_mode("Pause").has_value());
  assert(!parse_duck_mode("duck").has_value());
  assert(std::string(duck_mode_name(DuckMode::Pause)) == "pause");
}

// ---- parse_point --------------------------------------------------------------------------------

void test_parse_point() {
  auto p = parse_point("640,360");
  assert(p && p->first == 640 && p->second == 360);
  auto f = parse_point("12.5,7.25");
  assert(f && f->first == 12.5 && f->second == 7.25);

  // "" is the probe's "no such button" answer and must never parse to (0,0) — clicking the top-left
  // corner of the viewport because we could not find the mic is precisely the dead-button failure.
  assert(!parse_point("").has_value());
  assert(!parse_point("640").has_value());
  assert(!parse_point(",360").has_value());
  assert(!parse_point("640,").has_value());
  assert(!parse_point("a,b").has_value());
  assert(!parse_point("640,360x").has_value());
}

// ---- mic_probe_js -------------------------------------------------------------------------------

void test_mic_probe_js() {
  // Now config/scripts/mic_probe.js rendered with the selectors as a JSON string[] param.
  const std::string js = mic_probe_js({"[aria-label*='voice' i]", ".ytlr__mic"});
  // Selectors reach the page verbatim, in order, so a config hotfix needs no rebuild.
  assert(js.find("[aria-label*='voice' i]") != std::string::npos);
  assert(js.find(".ytlr__mic") != std::string::npos);
  assert(js.find("[aria-label*='voice' i]") < js.find(".ytlr__mic"));
  // ...and they arrive as the selectors param the script reads.
  assert(js.find("\"selectors\":[") != std::string::npos);
  // The body measures the rect (that is what makes the click land) and guards against a zero-area
  // element, and try/catches an invalid selector.
  assert(js.find("getBoundingClientRect") != std::string::npos);
  assert(js.find("!r.width || !r.height") != std::string::npos);
  assert(js.find("try {") != std::string::npos);

  // A double quote inside a selector must be escaped by ScriptParams, or the object literal breaks.
  const std::string q = mic_probe_js({"[title=\"x\"]"});
  assert(q.find("[title=\\\"x\\\"]") != std::string::npos);

  // No selectors: a well-formed program that finds nothing (an empty JSON array), not a syntax
  // error.
  const std::string empty = mic_probe_js({});
  assert(empty.find("\"selectors\":[]") != std::string::npos);
}

// ---- HoldToTalk ---------------------------------------------------------------------------------

void test_hold_requires_a_hold() {
  HoldToTalk h(250);
  assert(!h.active());

  // The press edge alone must NOT open the mic. input.cpp used to dispatch on the press edge and
  // discard releases entirely; if this ever returns Start, a stray tap opens the microphone.
  assert(h.on_press(1000) == HoldToTalk::Action::None);
  assert(!h.active());
  assert(h.pending());
  assert(h.deadline_ms() == 1250);

  // Before the deadline, nothing.
  assert(h.on_tick(1100) == HoldToTalk::Action::None);
  assert(h.on_tick(1249) == HoldToTalk::Action::None);
  assert(!h.active());

  // At the deadline it opens, exactly once.
  assert(h.on_tick(1250) == HoldToTalk::Action::Start);
  assert(h.active());
  assert(!h.pending());
  assert(h.on_tick(1300) == HoldToTalk::Action::None);  // no repeat Starts
  assert(h.on_tick(9999) == HoldToTalk::Action::None);

  // Release closes it, once.
  assert(h.on_release(1400) == HoldToTalk::Action::Stop);
  assert(!h.active());
  assert(h.on_release(1500) == HoldToTalk::Action::None);  // idempotent
}

void test_short_tap_never_opens_the_mic() {
  HoldToTalk h(250);
  h.on_press(1000);
  // Released after 100 ms: below the hold threshold. No Start was ever issued, so no Stop is owed.
  assert(h.on_release(1100) == HoldToTalk::Action::None);
  assert(!h.active());
  assert(!h.pending());
  // And a later tick must not resurrect it.
  assert(h.on_tick(2000) == HoldToTalk::Action::None);
  assert(!h.active());
}

void test_kernel_autorepeat_does_not_restart_the_hold() {
  // evdev sends value==2 for auto-repeat while held; input.cpp ignores it, but a duplicate press
  // from a merged second device (Steam wraps pads) must not reset the deadline either, or a held
  // button could never mature.
  HoldToTalk h(250);
  h.on_press(1000);
  assert(h.on_press(1200) == HoldToTalk::Action::None);
  assert(h.deadline_ms() == 1250);  // NOT 1450
  assert(h.on_tick(1250) == HoldToTalk::Action::Start);
}

void test_zero_hold_opens_on_the_first_tick() {
  HoldToTalk h(0);
  h.on_press(500);
  assert(h.on_tick(500) == HoldToTalk::Action::Start);
}

void test_release_without_press_is_inert() {
  HoldToTalk h(250);
  assert(h.on_release(1000) == HoldToTalk::Action::None);
  assert(!h.active());
}

// ---- VoiceController ----------------------------------------------------------------------------

VoiceConfig cfg_with(DuckMode duck, bool toggles = false) {
  VoiceConfig c;
  c.enabled = true;
  c.hold_ms = 250;
  c.duck = duck;
  c.click_toggles = toggles;
  c.mic_selectors = {"[aria-label*='voice' i]"};
  return c;
}

void test_start_clicks_the_button_centre_and_ducks() {
  testing::FakeServer server;
  VoiceController v("127.0.0.1", server.port(), cfg_with(DuckMode::Pause));
  server.take_requests();

  assert(v.start());
  assert(v.listening());
  auto r = server.take_requests();

  // Pressed at the rect centre the page reported (640,360) — not at a coordinate we invented.
  assert(sent(r, "\"method\":\"Input.dispatchMouseEvent\""));
  assert(sent(r, "\"type\":\"mousePressed\""));
  assert(sent(r, "\"x\":640"));
  assert(sent(r, "\"y\":360"));
  // Held down, not clicked: hold-to-talk keeps the button pressed until release.
  assert(!sent(r, "\"type\":\"mouseReleased\""));
  // And playback was paused BEFORE the mic opened (speaker bleed at ~15 cm — §13.3).
  assert(sent(r, "v.pause()"));

  v.stop();
  assert(!v.listening());
  r = server.take_requests();
  assert(sent(r, "\"type\":\"mouseReleased\""));
  assert(sent(r, "v.play()"));  // playback restored
}

void test_no_mic_button_clicks_nothing_and_ducks_nothing() {
  // The V0 failure, and the most important test in this file. If Leanback exposes no soft-mic
  // button under our Cobalt UA, we must not click a phantom coordinate and must not leave audio
  // ducked.
  testing::FakeServer server;
  server.set_mic_present(false);
  VoiceController v("127.0.0.1", server.port(), cfg_with(DuckMode::Pause));
  server.take_requests();

  assert(!v.start());
  assert(!v.listening());

  auto r = server.take_requests();
  assert(count_method(r, "Input.dispatchMouseEvent") == 0);  // nothing was clicked
  assert(!sent(r, "v.pause()"));                             // playback untouched

  // stop() after a failed start must be inert, not send a stray mouseReleased.
  v.stop();
  assert(count_method(server.take_requests(), "Input.dispatchMouseEvent") == 0);
}

void test_duck_none_leaves_playback_alone() {
  testing::FakeServer server;
  VoiceController v("127.0.0.1", server.port(), cfg_with(DuckMode::None));
  server.take_requests();

  assert(v.start());
  auto r = server.take_requests();
  assert(sent(r, "\"type\":\"mousePressed\""));  // the mic still opens
  assert(!sent(r, "v.pause()"));                 // but nothing touched the video
  assert(!sent(r, "v.muted"));

  v.stop();
  assert(!sent(server.take_requests(), "v.play()"));
}

void test_duck_mute_mutes_and_restores() {
  testing::FakeServer server;
  VoiceController v("127.0.0.1", server.port(), cfg_with(DuckMode::Mute));
  server.take_requests();

  assert(v.start());
  assert(sent(server.take_requests(), "v.muted = true"));
  v.stop();
  assert(sent(server.take_requests(), "v.muted = false"));
}

void test_click_toggle_mode_clicks_twice() {
  // If the page's control is tap-to-toggle rather than press-and-hold, holding the mouse button
  // down would never open it. Emulate hold by clicking again on release.
  testing::FakeServer server;
  VoiceController v("127.0.0.1", server.port(), cfg_with(DuckMode::None, /*toggles=*/true));
  server.take_requests();

  assert(v.start());
  auto r = server.take_requests();
  assert(sent(r, "\"type\":\"mousePressed\"") && sent(r, "\"type\":\"mouseReleased\""));
  assert(count_method(r, "Input.dispatchMouseEvent") == 2);  // a full click

  v.stop();
  assert(count_method(server.take_requests(), "Input.dispatchMouseEvent") == 2);  // and another
}

void test_start_is_idempotent_and_stop_when_idle_is_inert() {
  testing::FakeServer server;
  VoiceController v("127.0.0.1", server.port(), cfg_with(DuckMode::None));
  assert(v.start());
  server.take_requests();
  assert(v.start());  // already listening
  assert(count_method(server.take_requests(), "Input.dispatchMouseEvent") == 0);

  v.stop();
  server.take_requests();
  v.stop();  // already stopped
  assert(count_method(server.take_requests(), "Input.dispatchMouseEvent") == 0);
}

void test_engine_unreachable_fails_cleanly() {
  VoiceController v("127.0.0.1", 8, cfg_with(DuckMode::Pause));  // nothing listening on port 8
  assert(!v.start());
  assert(!v.listening());
  v.stop();  // must not crash or hang
}

}  // namespace

int main() {
  test_parse_duck_mode();
  test_parse_point();
  test_mic_probe_js();

  test_hold_requires_a_hold();
  test_short_tap_never_opens_the_mic();
  test_kernel_autorepeat_does_not_restart_the_hold();
  test_zero_hold_opens_on_the_first_tick();
  test_release_without_press_is_inert();

  test_start_clicks_the_button_centre_and_ducks();
  test_no_mic_button_clicks_nothing_and_ducks_nothing();
  test_duck_none_leaves_playback_alone();
  test_duck_mute_mutes_and_restores();
  test_click_toggle_mode_clicks_twice();
  test_start_is_idempotent_and_stop_when_idle_is_inert();
  test_engine_unreachable_fails_cleanly();

  std::puts("voice_test: all assertions passed");
  return 0;
}
