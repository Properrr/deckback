// L0 coverage of the touch gesture router's pure half: the gesture -> action mapping, and the
// decode of what the page hands back.
//
// The page script's recognition is covered by tests/js/touch_gestures.test.js. What is covered HERE
// is everything between that queue and a keypress -- the part that turns "the user flicked" into an
// actual trusted key, and the part that must not turn a transport failure into one.
//
// The decode's own trap: an empty queue and an unreadable answer are both "no gestures to act on",
// and only one of them means the page is fine. `ok` separates them, because a router that treats a
// dead engine as a quiet user retries nothing and reports nothing.
#include "gestures.hpp"

#include <cassert>
#include <cstdio>
#include <string>

#include "harness.hpp"

namespace deckback {
namespace {

void test_arrow_gestures_map_to_arrow_keys() {
  assert(gesture_key("arrow", "up") == "ArrowUp");
  assert(gesture_key("arrow", "down") == "ArrowDown");
  assert(gesture_key("arrow", "left") == "ArrowLeft");
  assert(gesture_key("arrow", "right") == "ArrowRight");
}

void test_back_and_tap_have_no_other_source() {
  // The left-edge swipe is the ONLY touch Back path (input-ux §7), and a tap is the A button.
  assert(gesture_key("back", "") == "Escape");
  assert(gesture_key("tap", "center") == "Enter");
  assert(gesture_key("tap", "left") == "Enter");
}

void test_unknown_and_non_key_gestures_map_to_no_key() {
  // Seek and hold are actions, not keys; a nonsense direction must not fall through to one either.
  assert(gesture_key("seek", "left").empty());
  assert(gesture_key("hold", "").empty());
  assert(gesture_key("arrow", "sideways").empty());
  assert(gesture_key("arrow", "").empty());
  assert(gesture_key("", "up").empty());
  assert(gesture_key("pinch", "in").empty());  // a script newer than this binary
}

void test_seek_sign() {
  assert(gesture_seek_sign("seek", 1) == 1);
  assert(gesture_seek_sign("seek", -1) == -1);
  assert(gesture_seek_sign("seek", 0) == 0);
  // Only a seek gesture seeks: an arrow carrying a stray numeric dir must not.
  assert(gesture_seek_sign("arrow", 1) == 0);
  assert(gesture_seek_sign("tap", -1) == 0);
}

void test_hold_rate_comes_from_the_zone() {
  // Left slows down, right and centre speed up. Both are RATES: making the left a rewind would have
  // meant stepped seeks on a timer, because Chromium has no negative playbackRate.
  assert(hold_rate_for_zone("left", 2.0, 0.5) == 0.5);
  assert(hold_rate_for_zone("right", 2.0, 0.5) == 2.0);
  assert(hold_rate_for_zone("center", 2.0, 0.5) == 2.0);
  // A script older than this binary sends no zone at all; it must speed up, not slow down.
  assert(hold_rate_for_zone("", 2.0, 0.5) == 2.0);
}

// The indicator shows the RUNNING TOTAL of a burst while each tap seeks a constant interval. An
// earlier version capped the multiplier at 6 "so a long run cannot cross a whole video" -- but the
// seek never used the multiplier at all, so the cap protected nothing and only made the indicator
// understate the real position from the seventh tap onward. A comment describing behaviour the code
// does not have is worse than no comment.
void test_the_indicator_tracks_the_whole_burst() {
  const int skip = 10;
  for (int n = 1; n <= 9; ++n)
    assert(seek_hud_text(skip * n) == "+" + std::to_string(10 * n) + " s");
  assert(seek_hud_text(-skip * 4) == "-40 s");
}

void test_hud_text_reads_at_a_glance() {
  // The ACCUMULATED total, so the user reads where they are going, not the increment.
  assert(seek_hud_text(30) == "+30 s");
  assert(seek_hud_text(-10) == "-10 s");
  assert(seek_hud_text(0).empty());
  // No trailing zeros at 34px.
  assert(hold_hud_text(2.0) == "2x");
  assert(hold_hud_text(0.5) == "0.5x");
  assert(hold_hud_text(0.25) == "0.25x");
}

void test_seek_accumulation_is_decoded() {
  GestureBatch b = parse_drain(R"({"configured":true,"q":[{"g":"seek","dir":1,"n":3}]})");
  assert(b.ok && b.gestures.size() == 1);
  assert(b.gestures[0].n == 3);
  // A script that sends no `n` predates accumulation: one step, never zero steps.
  GestureBatch b2 = parse_drain(R"({"configured":true,"q":[{"g":"seek","dir":1}]})");
  assert(b2.gestures[0].n == 1);
  GestureBatch b3 = parse_drain(R"({"configured":true,"q":[{"g":"seek","dir":1,"n":0}]})");
  assert(b3.gestures[0].n == 1);
}

void test_hold_zone_is_decoded() {
  GestureBatch b = parse_drain(R"({"configured":true,"q":[{"g":"hold","on":true,"zone":"left"}]})");
  assert(b.ok && b.gestures.size() == 1);
  assert(b.gestures[0].zone == "left" && b.gestures[0].on);
}

void test_hold_phase_distinguishes_press_from_release() {
  assert(gesture_hold_phase("hold", true) == 1);
  assert(gesture_hold_phase("hold", false) == -1);
  assert(gesture_hold_phase("tap", true) == 0);
}

void test_parse_drain_decodes_a_batch() {
  const std::string j =
      R"({"v":1,"configured":true,"enabled":true,"q":[)"
      R"({"g":"arrow","dir":"up"},{"g":"back"},{"g":"seek","dir":-1},{"g":"hold","on":true}]})";
  GestureBatch b = parse_drain(j);
  assert(b.ok);
  assert(b.configured);
  assert(b.enabled);
  assert(b.gestures.size() == 4);
  assert(b.gestures[0].kind == "arrow" && b.gestures[0].dir == "up");
  assert(b.gestures[1].kind == "back");
  // `dir` is a string for arrows and a number for seeks; both must survive the same field.
  assert(b.gestures[2].kind == "seek" && b.gestures[2].seek_dir == -1);
  assert(b.gestures[3].kind == "hold" && b.gestures[3].on);
}

void test_an_empty_queue_is_a_successful_read() {
  GestureBatch b = parse_drain(R"({"v":1,"configured":true,"enabled":true,"q":[]})");
  assert(b.ok);
  assert(b.gestures.empty());
  assert(b.configured);
}

void test_no_router_in_the_page_is_not_an_empty_queue() {
  // The script returns "" when window.__deckbackGestures is absent -- i.e. between a reload and the
  // document-start injection. Reporting ok would let the router conclude the page is healthy and
  // skip reconfiguring it.
  GestureBatch b = parse_drain("");
  assert(!b.ok);
  assert(b.gestures.empty());
}

void test_unusable_json_is_not_ok() {
  for (const char* bad : {"{ this is not json", "[]", "null", "\"a string\"", "{\"q\":5}"}) {
    GestureBatch b = parse_drain(bad);
    if (std::string(bad) == "{\"q\":5}") {
      // A well-formed object with a junk queue IS a successful read of a page that had nothing to
      // say -- the object parsed, so the router is alive.
      assert(b.ok);
      assert(b.gestures.empty());
      continue;
    }
    assert(!b.ok);
  }
}

void test_configured_defaults_false_so_a_reload_is_reconfigured() {
  // A fresh document-start copy reports configured:false and gets the user's thresholds pushed. If
  // a missing field defaulted to TRUE, every reload would silently run on built-in defaults.
  GestureBatch b = parse_drain(R"({"v":1,"q":[]})");
  assert(b.ok);
  assert(!b.configured);
}

void test_malformed_entries_are_skipped_not_guessed() {
  const std::string j =
      R"({"configured":true,"q":[{"g":"arrow","dir":"up"},{"dir":"up"},5,{"g":""},)"
      R"({"g":"tap","zone":"center"}]})";
  GestureBatch b = parse_drain(j);
  assert(b.ok);
  // The entry with no `g`, the bare number and the empty kind are dropped; the two valid ones stay.
  assert(b.gestures.size() == 2);
  assert(b.gestures[0].kind == "arrow");
  assert(b.gestures[1].kind == "tap");
}

void test_every_emitted_gesture_kind_has_an_action() {
  // The page script can emit exactly these. If one ever maps to nothing, a real finger movement
  // would silently do nothing on the Deck -- the failure this file exists to prevent.
  struct Case {
    const char* kind;
    const char* dir;
    int seek;
    bool on;
  };
  const Case cases[] = {
      {"arrow", "up", 0, false},    {"arrow", "down", 0, false}, {"arrow", "left", 0, false},
      {"arrow", "right", 0, false}, {"back", "", 0, false},      {"tap", "center", 0, false},
      {"seek", "", 1, false},       {"seek", "", -1, false},     {"hold", "", 0, true},
      {"hold", "", 0, false},
  };
  for (const Case& c : cases) {
    const bool handled = !gesture_key(c.kind, c.dir).empty() ||
                         gesture_seek_sign(c.kind, c.seek) != 0 ||
                         gesture_hold_phase(c.kind, c.on) != 0;
    assert(handled);
  }
}

}  // namespace
}  // namespace deckback

DECKBACK_TEST_MAIN(gestures) {
  using namespace deckback;
  test_arrow_gestures_map_to_arrow_keys();
  test_back_and_tap_have_no_other_source();
  test_unknown_and_non_key_gestures_map_to_no_key();
  test_seek_sign();
  test_hold_phase_distinguishes_press_from_release();
  test_hold_rate_comes_from_the_zone();
  test_hud_text_reads_at_a_glance();
  test_the_indicator_tracks_the_whole_burst();
  test_seek_accumulation_is_decoded();
  test_hold_zone_is_decoded();
  test_parse_drain_decodes_a_batch();
  test_an_empty_queue_is_a_successful_read();
  test_no_router_in_the_page_is_not_an_empty_queue();
  test_unusable_json_is_not_ok();
  test_configured_defaults_false_so_a_reload_is_reconfigured();
  test_malformed_entries_are_skipped_not_guessed();
  test_every_emitted_gesture_kind_has_an_action();
  std::puts("gestures_test: all assertions passed");
  return 0;
}
