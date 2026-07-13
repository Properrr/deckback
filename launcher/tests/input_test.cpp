// Unit tests for the Phase 3 input layer's pure logic: keymap resolution, direction resolution,
// button-map construction, and analog-trigger hysteresis. No evdev device and no CDP connection is
// needed — GamepadInput's I/O is deliberately kept out of these helpers.
#include "input.hpp"

#include <linux/input.h>

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "config.hpp"

using namespace deckback;

namespace {

int find_key(const std::vector<ButtonBinding>& v, int code, std::string* key) {
  for (const ButtonBinding& b : v) {
    if (b.code == code) {
      if (key) *key = b.key;
      return 1;
    }
  }
  return 0;
}

// ---- resolve_binding ----------------------------------------------------------------------------

void test_binding_accepts_dom_keys_verbatim() {
  // A DOM key passes straight through. This is what makes app.json hot-swappable: a Leanback change
  // can be worked around by editing config, without a launcher rebuild.
  assert(resolve_binding("Enter") == "Enter");
  assert(resolve_binding("Escape") == "Escape");
  assert(resolve_binding("ArrowUp") == "ArrowUp");
  assert(resolve_binding("MediaPlayPause") == "MediaPlayPause");
  assert(resolve_binding("c") == "c");
}

void test_binding_translates_semantic_actions() {
  assert(resolve_binding("select") == "Enter");
  assert(resolve_binding("back") == "Escape");
  assert(resolve_binding("playpause") == "MediaPlayPause");
  assert(resolve_binding("toggle_captions") == "c");
  // These dispatch arrows, which scrub the progress bar — they are not a fixed-interval seek.
  assert(resolve_binding("scrub_back") == "ArrowLeft");
  assert(resolve_binding("scrub_fwd") == "ArrowRight");
}

void test_deprecated_seek_aliases_still_resolve() {
  // A remotely hot-swapped app.json written against the old names must keep working for one release
  // (doc §6 R1), so these resolve to exactly what they always did...
  assert(resolve_binding("seek_back_10") == "ArrowLeft");
  assert(resolve_binding("seek_fwd_10") == "ArrowRight");
  // ...but they are reported as deprecated so startup can warn instead of staying silent.
  assert(deprecated_action_replacement("seek_back_10") == "scrub_back");
  assert(deprecated_action_replacement("seek_fwd_10") == "scrub_fwd");

  // Current names, DOM keys, and unknown values are not deprecated.
  assert(deprecated_action_replacement("scrub_back").empty());
  assert(deprecated_action_replacement("select").empty());
  assert(deprecated_action_replacement("ArrowLeft").empty());
  assert(deprecated_action_replacement("c").empty());
  assert(deprecated_action_replacement("voice_search").empty());
  assert(deprecated_action_replacement("").empty());
}

// The scan keys became dispatchable when devtools.cpp learned MediaRewind/MediaFastForward. That
// does NOT bind them: Leanback's handling of those codes is unverified, so the aliases stay
// unmapped until the on-Deck spike says otherwise. A user can still opt in via app.json by naming
// the DOM key.
void test_scan_keys_dispatchable_but_still_unbound() {
  assert(resolve_binding("scan_rewind").empty());
  assert(resolve_binding("scan_forward").empty());
  assert(resolve_binding("MediaRewind") == "MediaRewind");
  assert(resolve_binding("MediaFastForward") == "MediaFastForward");
}

void test_binding_refuses_to_guess_unknown_actions() {
  // These appear in config/app.json but have no established Leanback key. Dispatching *something*
  // would look like it works while doing nothing; an empty result makes startup warn instead.
  assert(resolve_binding("voice_search").empty());
  assert(resolve_binding("player_menu").empty());
  assert(resolve_binding("scan_rewind").empty());
  assert(resolve_binding("scan_forward").empty());
  assert(resolve_binding("navigate").empty());  // handled by the D-pad/stick path, not a button
  assert(resolve_binding("").empty());
  assert(resolve_binding("NoSuchKey").empty());
}

// ---- resolve_direction --------------------------------------------------------------------------

void test_direction_neutral_is_null() { assert(resolve_direction(0, 0, 0, 0) == nullptr); }

void test_direction_vertical_beats_horizontal() {
  // Diagonal input must resolve to exactly one arrow, and vertical wins (menus are vertical lists).
  assert(std::string(resolve_direction(1, -1, 0, 0)) == "ArrowUp");
  assert(std::string(resolve_direction(-1, 1, 0, 0)) == "ArrowDown");
}

void test_direction_dpad_beats_stick() {
  // hat says left, stick says right -> left.
  assert(std::string(resolve_direction(-1, 0, 1, 0)) == "ArrowLeft");
  // hat neutral -> stick is used.
  assert(std::string(resolve_direction(0, 0, 1, 0)) == "ArrowRight");
  assert(std::string(resolve_direction(0, 0, 0, -1)) == "ArrowUp");
}

void test_direction_returns_stable_pointers() {
  // GamepadInput::set_direction compares by pointer identity to detect "same key still held".
  assert(resolve_direction(0, -1, 0, 0) == resolve_direction(0, -1, 0, 0));
  assert(resolve_direction(0, -1, 0, 0) != resolve_direction(0, 1, 0, 0));
}

// ---- build_button_map ---------------------------------------------------------------------------

void test_button_map_from_shipped_keymap() {
  // Exactly the keymap in config/app.json.
  const std::vector<std::pair<std::string, std::string>> keymap = {
      {"dpad", "navigate"},
      {"a", "select"},
      {"b", "back"},
      {"x", "playpause"},
      {"y", "voice_search"},
      {"lb", "scrub_back"},
      {"rb", "scrub_fwd"},
      {"lt", "scan_rewind"},
      {"rt", "scan_forward"},
      {"start", "show_controls"},
      {"select", "toggle_captions"},
  };
  std::vector<std::string> unmapped;
  auto m = build_button_map(keymap, &unmapped);

  std::string k;
  assert(find_key(m, BTN_SOUTH, &k) && k == "Enter");
  assert(find_key(m, BTN_EAST, &k) && k == "Escape");
  assert(find_key(m, BTN_X, &k) && k == "MediaPlayPause");  // X (BTN_X == BTN_NORTH)
  assert(find_key(m, BTN_TL, &k) && k == "ArrowLeft");      // LB
  assert(find_key(m, BTN_TR, &k) && k == "ArrowRight");     // RB
  assert(find_key(m, BTN_SELECT, &k) && k == "c");          // captions

  // Y and Start dispatch no DOM key -> not bound, and reported. Both are *launcher* actions
  // (voice_search, show_controls); build_button_map does not know that, so it honestly reports them
  // as unmapped and the constructor removes each one only when its feature is actually enabled.
  assert(!find_key(m, BTN_Y, nullptr));  // Y (BTN_Y == BTN_WEST) -> voice_search, a launcher action
  assert(!find_key(m, BTN_START, nullptr));
  // dpad/lt/rt are not EV_KEY buttons and must never appear here.
  assert(m.size() == 6);

  // y, start reported (lt/rt are handled by the trigger path, not build_button_map).
  assert(unmapped.size() == 2);
}

void test_button_map_reports_unknown_control_names() {
  std::vector<std::string> unmapped;
  auto m = build_button_map({{"nope", "select"}, {"a", "AlsoNotAKey"}}, &unmapped);
  assert(m.empty());
  assert(unmapped.size() == 2);
}

void test_button_map_accepts_direct_dom_keys() {
  // The hot-swap path: rebind A to Escape without touching C++.
  std::vector<std::string> unmapped;
  auto m = build_button_map({{"a", "Escape"}, {"b", "MediaTrackNext"}}, &unmapped);
  std::string k;
  assert(find_key(m, BTN_SOUTH, &k) && k == "Escape");
  assert(find_key(m, BTN_EAST, &k) && k == "MediaTrackNext");
  assert(unmapped.empty());
}

void test_button_map_tolerates_empty_keymap() {
  std::vector<std::string> unmapped;
  auto m = build_button_map({}, &unmapped);
  assert(m.empty());
  assert(unmapped.empty());
}

// ---- trigger_pressed ----------------------------------------------------------------------------

void test_trigger_hysteresis() {
  // Rising edge needs a firm pull.
  assert(!trigger_pressed(0, false));
  assert(!trigger_pressed(191, false));
  assert(trigger_pressed(192, false));
  assert(trigger_pressed(255, false));

  // Once held, it stays held until it falls well back — a trigger resting at ~150 must not chatter.
  assert(trigger_pressed(150, true));
  assert(trigger_pressed(65, true));
  assert(!trigger_pressed(64, true));
  assert(!trigger_pressed(0, true));
}

// ---- fixed-interval skip (input-ux §18) ---------------------------------------------------------

void test_skip_action_sign() {
  // Forward and back, plus the longer spellings a hot-swapped app.json might use.
  assert(skip_action_sign("skip_fwd") == 1);
  assert(skip_action_sign("skip_forward") == 1);
  assert(skip_action_sign("skip_back") == -1);
  assert(skip_action_sign("skip_backward") == -1);
  // Everything else — including the scrub actions and DOM keys — is NOT a skip.
  assert(skip_action_sign("scrub_fwd") == 0);
  assert(skip_action_sign("scrub_back") == 0);
  assert(skip_action_sign("ArrowRight") == 0);
  assert(skip_action_sign("") == 0);
}

void test_build_skip_js_carries_the_signed_delta() {
  // The delta appears verbatim (with sign) and the seek prefers the player's own seekBy().
  const std::string fwd = build_skip_js(10);
  assert(fwd.find("var d=10;") != std::string::npos);
  assert(fwd.find("p.seekBy(d,true)") != std::string::npos);
  assert(fwd.find("#movie_player") != std::string::npos);

  const std::string back = build_skip_js(-10);
  assert(back.find("var d=-10;") != std::string::npos);
  // A negative jump must not run off the front of the media: the <video> fallback clamps at 0.
  assert(back.find("Math.max(0,v.currentTime+d)") != std::string::npos);
}

// ---- parse_chord --------------------------------------------------------------------------------

void test_parse_chord_valid() {
  // Default touch-lock chord: both stick clicks.
  auto c = parse_chord("l3+r3");
  assert(c.first == BTN_THUMBL && c.second == BTN_THUMBR);
  // Case/whitespace tolerant, and any two distinct controls work.
  auto d = parse_chord(" Select + Start ");
  assert(d.first == BTN_SELECT && d.second == BTN_START);
}

void test_parse_chord_rejects_bad_input() {
  assert((parse_chord("l3") == std::pair<int, int>{-1, -1}));       // no '+'
  assert((parse_chord("l3+l3") == std::pair<int, int>{-1, -1}));    // same control twice
  assert((parse_chord("l3+nope") == std::pair<int, int>{-1, -1}));  // unknown control
  assert((parse_chord("") == std::pair<int, int>{-1, -1}));
}

// ---- config keymap parsing ----------------------------------------------------------------------

void test_config_parses_keymap_object() {
  // The parser is a hand-rolled top-level extractor; the keymap is its first nested object.
  const char* json = R"({
    "url": "https://www.youtube.com/tv",
    "keymap": {
      "$comment": "skip me, and my : colon and , comma",
      "a": "select",
      "b": "back",
      "select": "toggle_captions"
    },
    "remote_debugging_port": 9222
  })";
  const std::string path = "/tmp/deckback_input_test_cfg.json";
  FILE* f = std::fopen(path.c_str(), "w");
  assert(f);
  std::fputs(json, f);
  std::fclose(f);

  auto cfg = Config::load(path);
  assert(cfg.has_value());
  // $comment must not become a binding, and a later top-level key must not leak in.
  assert(cfg->keymap.size() == 3);
  assert(cfg->keymap[0].first == "a" && cfg->keymap[0].second == "select");
  assert(cfg->keymap[2].first == "select" && cfg->keymap[2].second == "toggle_captions");
  // The keys after the nested object still parse.
  assert(cfg->remote_debugging_port == 9222);
  std::remove(path.c_str());
}

void test_config_missing_keymap_is_empty_not_fatal() {
  const char* json = R"({"url":"https://x","remote_debugging_port":1})";
  const std::string path = "/tmp/deckback_input_test_cfg2.json";
  FILE* f = std::fopen(path.c_str(), "w");
  assert(f);
  std::fputs(json, f);
  std::fclose(f);
  auto cfg = Config::load(path);
  assert(cfg.has_value());
  assert(cfg->keymap.empty());
  std::remove(path.c_str());
}

void test_config_parses_touch_settings() {
  const char* json = R"({
    "url": "https://x",
    "disable_touch": false,
    "block_touchscreen": true,
    "touch_lock_enabled": false,
    "touch_lock_chord": "select+start",
    "touch_lock_unlock_hold_ms": 1200,
    "touch_lock_toast": false,
    "touch_lock_haptic": false
  })";
  const std::string path = "/tmp/deckback_input_test_cfg3.json";
  FILE* f = std::fopen(path.c_str(), "w");
  assert(f);
  std::fputs(json, f);
  std::fclose(f);
  auto cfg = Config::load(path);
  assert(cfg.has_value());
  assert(cfg->disable_touch == false);  // overridable off
  assert(cfg->block_touchscreen == true);
  assert(cfg->touch_lock_enabled == false);
  assert(cfg->touch_lock_chord == "select+start");
  assert(cfg->touch_lock_unlock_hold_ms == 1200);
  assert(cfg->touch_lock_toast == false);
  assert(cfg->touch_lock_haptic == false);
  std::remove(path.c_str());
}

// Defaults when the keys are absent: touch disabled (A+B), and the dead EVIOCGRAB lock ships OFF so
// the onboarding card never advertises it (findings durable/touch-lock.md).
void test_config_touch_defaults() {
  const char* json = R"({"url":"https://x"})";
  const std::string path = "/tmp/deckback_input_test_cfg4.json";
  FILE* f = std::fopen(path.c_str(), "w");
  assert(f);
  std::fputs(json, f);
  std::fclose(f);
  auto cfg = Config::load(path);
  assert(cfg.has_value());
  assert(cfg->disable_touch == true);  // touch is inert by default
  assert(cfg->block_touchscreen == false);
  assert(cfg->touch_lock_enabled == false);  // dead lock retired
  assert(cfg->touch_lock_chord == "l3+r3");
  assert(cfg->touch_lock_unlock_hold_ms == 800);
  assert(cfg->touch_lock_toast == true);
  assert(cfg->touch_lock_haptic == true);
  // Right stick on by default: the axis is otherwise dead, and arrows are the only Leanback keys we
  // have actually verified, so this adds no new assumption.
  assert(cfg->right_stick_scroll == true);
  assert(cfg->right_stick_deadzone == 13000);
  assert(cfg->right_stick_slow_ms == 200);
  assert(cfg->right_stick_fast_ms == 45);
  std::remove(path.c_str());
}

void test_config_parses_right_stick() {
  const char* json = R"({
    "url": "https://x",
    "right_stick_scroll": false,
    "right_stick_deadzone": 9000,
    "right_stick_slow_ms": 300,
    "right_stick_fast_ms": 60
  })";
  const std::string path = "/tmp/deckback_input_test_cfg5.json";
  FILE* f = std::fopen(path.c_str(), "w");
  assert(f);
  std::fputs(json, f);
  std::fclose(f);
  auto cfg = Config::load(path);
  assert(cfg.has_value());
  assert(cfg->right_stick_scroll == false);
  assert(cfg->right_stick_deadzone == 9000);
  assert(cfg->right_stick_slow_ms == 300);
  assert(cfg->right_stick_fast_ms == 60);
  std::remove(path.c_str());
}

void test_config_skip_seconds() {
  // Present: parsed. Absent: the default 10 s stands (a hot-swapped app.json need not carry it).
  const char* json = R"({ "url": "https://x", "skip_seconds": 30 })";
  const std::string path = "/tmp/deckback_input_test_skip.json";
  FILE* f = std::fopen(path.c_str(), "w");
  assert(f);
  std::fputs(json, f);
  std::fclose(f);
  auto cfg = Config::load(path);
  assert(cfg.has_value());
  assert(cfg->skip_seconds == 30);
  std::remove(path.c_str());

  const char* json2 = R"({ "url": "https://x" })";
  const std::string path2 = "/tmp/deckback_input_test_skip2.json";
  FILE* g = std::fopen(path2.c_str(), "w");
  assert(g);
  std::fputs(json2, g);
  std::fclose(g);
  auto cfg2 = Config::load(path2);
  assert(cfg2.has_value());
  assert(cfg2->skip_seconds == 10);
  std::remove(path2.c_str());
}

// ---- fast_scroll (right stick) ------------------------------------------------------------------

void test_fast_scroll_idle_inside_the_deadzone() {
  FastScrollConfig c;
  assert(fast_scroll(0, 0, c).key == nullptr);
  assert(fast_scroll(0, 0, c).interval_ms == 0);
  assert(fast_scroll(13000, 13000, c).key == nullptr);  // exactly at the edge is still idle
  assert(fast_scroll(-13000, 0, c).key == nullptr);
  // Just past it, the stick scrolls.
  assert(fast_scroll(0, 13001, c).key != nullptr);
}

void test_fast_scroll_disabled_is_always_idle() {
  FastScrollConfig c;
  c.enabled = false;
  assert(fast_scroll(32767, -32767, c).key == nullptr);
}

void test_fast_scroll_directions() {
  FastScrollConfig c;
  // evdev Y is inverted: negative is up.
  assert(std::string(fast_scroll(0, -30000, c).key) == "ArrowUp");
  assert(std::string(fast_scroll(0, 30000, c).key) == "ArrowDown");
  assert(std::string(fast_scroll(-30000, 0, c).key) == "ArrowLeft");
  assert(std::string(fast_scroll(30000, 0, c).key) == "ArrowRight");
  // Stable module constants, so GamepadInput can compare by identity.
  assert(fast_scroll(0, 30000, c).key == fast_scroll(0, 20000, c).key);
}

void test_fast_scroll_dominant_axis_wins_ties_go_vertical() {
  FastScrollConfig c;
  // Unlike the digital resolve_direction, magnitude is meaningful here: a diagonal push means
  // whichever way the thumb pushed harder.
  assert(std::string(fast_scroll(30000, -20000, c).key) == "ArrowRight");
  assert(std::string(fast_scroll(20000, -30000, c).key) == "ArrowUp");
  // A perfect diagonal resolves vertical, the way rails run.
  assert(std::string(fast_scroll(25000, 25000, c).key) == "ArrowDown");
  // The rate comes from the *dominant* axis, not the other one. A hard right push with a slight
  // vertical lean must scroll at the hard-push rate.
  assert(fast_scroll(32767, 14000, c).interval_ms == fast_scroll(32767, 0, c).interval_ms);
}

void test_fast_scroll_rate_is_analog_not_timed() {
  FastScrollConfig c;  // slow 200, fast 45
  // The whole point of the feature: deflection sets the rate. A nudge steps deliberately; a full
  // push scrolls fast. If these two are ever equal, the right stick is just a slow D-pad.
  const long nudge = fast_scroll(0, 14000, c).interval_ms;
  const long half = fast_scroll(0, 23000, c).interval_ms;
  const long full = fast_scroll(0, 32767, c).interval_ms;
  assert(nudge > half && half > full);
  assert(nudge <= 200 && nudge > 180);  // just past the deadzone ~ slow_ms
  assert(full == 45);                   // full deflection == fast_ms exactly
}

void test_fast_scroll_clamps_the_axis_minimum() {
  // The axis floor is -32768, one past the +32767 ceiling. The observable consequence of not
  // clamping is NOT the interval (the lo/hi clamp already catches that) — it is the tie-break:
  // |-32768| > |32767|, so a full down-left push would resolve Left instead of Down, and pushing a
  // stick to its physical corner would scroll a different way depending on which corner.
  FastScrollConfig c;
  assert(std::string(fast_scroll(-32768, 32767, c).key) == "ArrowDown");
  assert(std::string(fast_scroll(-32768, -32768, c).key) == "ArrowUp");
  assert(std::string(fast_scroll(32767, -32768, c).key) == "ArrowUp");

  // Same at the top: a pad reporting past +32767 saturates rather than out-voting a full-deflection
  // vertical push. Both axes at full is a tie, whatever the device claims.
  assert(std::string(fast_scroll(40000, 32767, c).key) == "ArrowDown");
  assert(fast_scroll(40000, 0, c).interval_ms == fast_scroll(32767, 0, c).interval_ms);

  // And the rate at the floor equals the rate at the ceiling: both are full deflection.
  assert(fast_scroll(0, -32768, c).interval_ms == fast_scroll(0, 32767, c).interval_ms);
  assert(fast_scroll(0, -32768, c).interval_ms == 45);
  assert(std::string(fast_scroll(0, -32768, c).key) == "ArrowUp");
}

void test_fast_scroll_never_returns_a_busy_looping_interval() {
  // A hot-swapped app.json is not trusted input. Every one of these must still yield an interval a
  // poll loop can survive: never zero, never negative.
  FastScrollConfig c;
  c.slow_ms = 10;
  c.fast_ms = 10;
  assert(fast_scroll(0, 32767, c).interval_ms >= 25);  // both below the CDP floor

  c.slow_ms = 0;
  c.fast_ms = 0;
  assert(fast_scroll(0, 20000, c).interval_ms >= 25);

  c.slow_ms = -500;  // nonsense
  c.fast_ms = -100;
  assert(fast_scroll(0, 20000, c).interval_ms >= 25);

  // Inverted (fast slower than slow) is a preference, not an error: it must stay inside its own
  // range rather than extrapolate off the end.
  c.slow_ms = 50;
  c.fast_ms = 300;
  const long edge = fast_scroll(0, 14000, c).interval_ms;
  const long deep = fast_scroll(0, 32767, c).interval_ms;
  assert(edge >= 50 && edge <= 300);
  assert(deep == 300);
  assert(deep > edge);
}

void test_fast_scroll_degenerate_deadzone() {
  // A deadzone at or past full deflection swallows the axis: the stick is simply off. It must not
  // divide by a zero (or negative) span on the way to saying so.
  FastScrollConfig c;
  c.deadzone = 32767;
  assert(fast_scroll(0, 32767, c).key == nullptr);
  assert(fast_scroll(0, -32768, c).key == nullptr);
  c.deadzone = 99999;
  assert(fast_scroll(0, 32767, c).key == nullptr);

  // A negative deadzone must mean zero, not a widened rate ramp. Untreated, the ramp normalises
  // over [deadzone, 32767] and a barely-touched stick lands mid-ramp — the faintest brush would
  // scroll at half speed. Assert the rate, not merely that nothing crashed.
  FastScrollConfig zero = c;
  zero.deadzone = 0;
  c.deadzone = -32767;
  const FastScrollTick t = fast_scroll(0, 1, c);
  assert(t.key != nullptr);
  assert(t.interval_ms == fast_scroll(0, 1, zero).interval_ms);
  assert(t.interval_ms == 200);  // slow_ms: a 1-unit deflection is the slowest step, not the middle
}

// ---- TouchLockChord ----------------------------------------------------------------------------

// The asymmetry is the point: locking is cheap and instant, unlocking must be deliberate.
void test_chord_locks_immediately() {
  TouchLockChord c(800);
  assert(!c.locked());
  assert(c.on_chord(true, 1000) == TouchLockChord::Action::Lock);
  assert(c.locked());
  assert(!c.pending());
  // Continuing to hold must NOT then unlock: one action per engagement.
  assert(c.on_chord(true, 1900) == TouchLockChord::Action::None);
  assert(c.on_tick(2000) == TouchLockChord::Action::None);
  assert(c.locked());
}

void test_chord_unlock_requires_a_deliberate_hold() {
  TouchLockChord c(800);
  c.set_locked(true);

  // The press alone must not unlock. If this ever returns Unlock, a thumb resting on both sticks
  // hands the touchscreen back to the palm already on the panel — the failure the lock prevents.
  assert(c.on_chord(true, 1000) == TouchLockChord::Action::None);
  assert(c.locked());
  assert(c.pending());
  assert(c.deadline_ms() == 1800);

  assert(c.on_tick(1799) == TouchLockChord::Action::None);
  assert(c.locked());
  assert(c.on_tick(1800) == TouchLockChord::Action::Unlock);
  assert(!c.locked());
  assert(!c.pending());

  // Consumed: still holding the chord must not immediately re-lock.
  assert(c.on_tick(1900) == TouchLockChord::Action::None);
  assert(c.on_chord(true, 2000) == TouchLockChord::Action::None);
  assert(!c.locked());
}

void test_chord_released_early_leaves_the_lock_engaged() {
  TouchLockChord c(800);
  c.set_locked(true);
  c.on_chord(true, 1000);
  assert(c.pending());
  assert(c.on_chord(false, 1400) == TouchLockChord::Action::None);  // let go at 400 ms
  assert(c.locked());
  assert(!c.pending());
  // And a later tick must not resurrect the abandoned hold.
  assert(c.on_tick(2000) == TouchLockChord::Action::None);
  assert(c.locked());
}

void test_chord_repeat_reports_do_not_restart_the_unlock_hold() {
  // evdev sends value==2 for autorepeat while held, and Steam merges pads so one press can arrive
  // from two nodes. Re-arming on each would push the deadline out forever and the unlock would
  // never mature.
  TouchLockChord c(800);
  c.set_locked(true);
  c.on_chord(true, 1000);
  assert(c.on_chord(true, 1500) == TouchLockChord::Action::None);
  assert(c.deadline_ms() == 1800);  // NOT 2300
  assert(c.on_chord(true, 1800) == TouchLockChord::Action::Unlock);
}

void test_chord_relock_after_unlock_needs_a_fresh_press() {
  TouchLockChord c(800);
  assert(c.on_chord(true, 0) == TouchLockChord::Action::Lock);
  c.on_chord(false, 100);  // release
  assert(c.on_chord(true, 200) == TouchLockChord::Action::None);
  assert(c.on_tick(1000) == TouchLockChord::Action::Unlock);
  c.on_chord(false, 1100);
  assert(c.on_chord(true, 1200) == TouchLockChord::Action::Lock);  // full cycle
  assert(c.locked());
}

void test_chord_zero_hold_unlocks_on_the_press() {
  // Configurable back to the old toggle behaviour, for a user who wants it.
  TouchLockChord c(0);
  c.set_locked(true);
  assert(c.on_chord(true, 500) == TouchLockChord::Action::Unlock);
  assert(!c.locked());
}

void test_chord_negative_hold_is_clamped_not_undefined() {
  TouchLockChord c(-1);
  c.set_locked(true);
  assert(c.on_chord(true, 500) == TouchLockChord::Action::Unlock);
}

void test_chord_set_locked_reconciles_a_failed_grab() {
  // EVIOCGRAB can fail (gamescope may hold the panel). The machine must never claim a lock the
  // kernel refused, or the next chord press would try to *unlock* a screen that was never locked.
  TouchLockChord c(800);
  assert(c.on_chord(true, 1000) == TouchLockChord::Action::Lock);
  c.set_locked(false);  // the grab failed; caller reconciles
  assert(!c.locked());
  c.on_chord(false, 1100);
  assert(c.on_chord(true, 1200) ==
         TouchLockChord::Action::Lock);  // tries to lock again, not unlock
}

void test_chord_never_acts_while_not_fully_held() {
  TouchLockChord c(800);
  assert(c.on_chord(false, 1000) == TouchLockChord::Action::None);
  assert(c.on_tick(2000) == TouchLockChord::Action::None);
  assert(!c.locked());
  assert(!c.pending());
}

// ---- layers
// --------------------------------------------------------------------------------------

void test_resolve_layer() {
  assert(resolve_layer(false, false) == Layer::Browse);
  assert(resolve_layer(true, false) == Layer::Player);
  assert(resolve_layer(false, true) == Layer::Osk);
  // Typing outranks transport: a search field can be focused over a playing video, and there every
  // key must mean a character or an edit.
  assert(resolve_layer(true, true) == Layer::Osk);
}

// The base map alone must behave exactly as it did before layers existed.
void test_layers_absent_is_context_free() {
  Keymaps m =
      build_keymaps(KeymapConfig{{{"a", "select"}, {"b", "back"}}, {}, {}, {}, {}}, nullptr);
  for (Layer l : {Layer::Browse, Layer::Player, Layer::Osk}) {
    assert(resolve_button(m, BTN_SOUTH, l, false, false) == "Enter");
    assert(resolve_button(m, BTN_EAST, l, false, false) == "Escape");
    assert(resolve_button(m, BTN_NORTH, l, false, false).empty());  // unbound stays unbound
    assert(resolve_direction_key(m, "ArrowLeft", l, false, false) == "ArrowLeft");
  }
}

void test_context_layer_overrides_then_falls_through() {
  KeymapConfig cfg;
  cfg.base = {{"a", "select"}, {"b", "back"}, {"select", "toggle_captions"}};
  cfg.osk = {{"b", "Delete"}};  // inside the OSK, B erases rather than leaving search
  Keymaps m = build_keymaps(cfg, nullptr);

  // Browse/Player keep the base binding for B...
  assert(resolve_button(m, BTN_EAST, Layer::Browse, false, false) == "Escape");
  assert(resolve_button(m, BTN_EAST, Layer::Player, false, false) == "Escape");
  // ...the OSK overrides it.
  assert(resolve_button(m, BTN_EAST, Layer::Osk, false, false) == "Delete");
  // A control the layer does not bind falls through to base — a context layer only overrides.
  assert(resolve_button(m, BTN_SOUTH, Layer::Osk, false, false) == "Enter");
  assert(resolve_button(m, BTN_SELECT, Layer::Osk, false, false) == "c");
}

void test_modifier_layer_wins_and_absorbs() {
  KeymapConfig cfg;
  cfg.base = {{"a", "select"}, {"b", "back"}};
  cfg.lt_mod = {{"left", "MediaRewind"}, {"right", "MediaFastForward"}};
  Keymaps m = build_keymaps(cfg, nullptr);

  // Not held: plain arrows and plain buttons.
  assert(resolve_direction_key(m, "ArrowLeft", Layer::Browse, false, false) == "ArrowLeft");
  assert(resolve_button(m, BTN_SOUTH, Layer::Browse, false, false) == "Enter");

  // LT held: the layer's key, not the arrow.
  assert(resolve_direction_key(m, "ArrowLeft", Layer::Browse, true, false) == "MediaRewind");
  assert(resolve_direction_key(m, "ArrowRight", Layer::Browse, true, false) == "MediaFastForward");

  // A modifier layer ABSORBS what it does not bind. Falling through would fire a plain A/Up while
  // the user is holding a modifier precisely to mean "not the normal action".
  assert(resolve_button(m, BTN_SOUTH, Layer::Browse, true, false).empty());
  assert(resolve_direction_key(m, "ArrowUp", Layer::Browse, true, false).empty());

  // A modifier outranks the context layer.
  cfg.player = {{"left", "ArrowUp"}};
  m = build_keymaps(cfg, nullptr);
  assert(resolve_direction_key(m, "ArrowLeft", Layer::Player, false, false) == "ArrowUp");
  assert(resolve_direction_key(m, "ArrowLeft", Layer::Player, true, false) == "MediaRewind");
}

void test_empty_modifier_layer_is_not_a_modifier() {
  // With no keymap_lt configured, holding LT must change nothing — the trigger keeps its own
  // binding and the rest of the pad behaves normally. Otherwise a resting trigger would eat every
  // button.
  Keymaps m = build_keymaps(KeymapConfig{{{"a", "select"}}, {}, {}, {}, {}}, nullptr);
  assert(resolve_button(m, BTN_SOUTH, Layer::Browse, true, true) == "Enter");
  assert(resolve_direction_key(m, "ArrowUp", Layer::Browse, true, true) == "ArrowUp");
}

void test_lt_wins_over_rt_when_both_held() {
  KeymapConfig cfg;
  cfg.base = {{"a", "select"}};
  cfg.lt_mod = {{"a", "MediaRewind"}};
  cfg.rt_mod = {{"a", "MediaFastForward"}};
  Keymaps m = build_keymaps(cfg, nullptr);
  assert(resolve_button(m, BTN_SOUTH, Layer::Browse, true, true) == "MediaRewind");
  assert(resolve_button(m, BTN_SOUTH, Layer::Browse, false, true) == "MediaFastForward");
}

void test_build_layer_reports_unmapped_with_layer_label() {
  std::vector<std::string> unmapped;
  KeymapConfig cfg;
  cfg.base = {{"a", "select"}};
  cfg.osk = {{"b", "no_such_action"}, {"up", "also_bogus"}};
  build_keymaps(cfg, &unmapped);
  assert(unmapped.size() == 2);
  // The message must name the layer, or a warning about "b" is unactionable across five sections.
  for (const std::string& u : unmapped) assert(u.rfind("keymap_osk.", 0) == 0);
}

void test_empty_base_keeps_builtin_defaults() {
  Keymaps m = build_keymaps(KeymapConfig{}, nullptr);
  assert(resolve_button(m, BTN_SOUTH, Layer::Browse, false, false) == "Enter");
  assert(resolve_button(m, BTN_EAST, Layer::Browse, false, false) == "Escape");
}

// ---- voice control resolution
// --------------------------------------------------------------------

void test_find_voice_control() {
  // voice_search is not a DOM key (input-ux §8.2), so the input layer intercepts its control by
  // evdev code rather than looking it up in the button map.
  assert(find_control_for_action({{"y", "voice_search"}, {"a", "select"}}, "voice_search") ==
         BTN_Y);
  assert(find_control_for_action({{"start", "voice_search"}}, "voice_search") == BTN_START);
  // Absent, or bound to a control we do not know: -1, and startup warns rather than guessing.
  assert(find_control_for_action({{"a", "select"}}, "voice_search") == -1);
  assert(find_control_for_action({{"nosuch", "voice_search"}}, "voice_search") == -1);
  assert(find_control_for_action({}, "voice_search") == -1);

  // The same mechanism carries `show_controls`, the other launcher-performed action.
  assert(find_control_for_action({{"start", "show_controls"}}, "show_controls") == BTN_START);
  // ...and the two must not be confused for one another.
  assert(find_control_for_action({{"y", "voice_search"}}, "show_controls") == -1);
}

void test_without_voice_control() {
  auto out =
      without_action({{"a", "select"}, {"y", "voice_search"}, {"b", "back"}}, "voice_search");
  assert(out.size() == 2);
  assert(out[0].first == "a" && out[1].first == "b");  // order otherwise preserved
  assert(without_action({{"a", "select"}}, "voice_search").size() == 1);
  // Removing one action leaves the other alone.
  auto both = without_action({{"y", "voice_search"}, {"start", "show_controls"}}, "voice_search");
  assert(both.size() == 1 && both[0].second == "show_controls");
}

// With voice enabled, its control must not ALSO be reported as an unmapped binding: it is mapped,
// to something that is not a key. With voice disabled it must still be reported, because then it
// really is a dead control and the startup warning is the honest output.
void test_voice_control_is_not_reported_unmapped_when_enabled() {
  const std::vector<std::pair<std::string, std::string>> keymap = {
      {"a", "select"}, {"y", "voice_search"}, {"start", "player_menu"}};

  std::vector<std::string> off;
  build_keymaps(KeymapConfig{keymap, {}, {}, {}, {}}, &off);
  assert(off.size() == 2);  // y and start

  std::vector<std::string> on;
  build_keymaps(KeymapConfig{without_action(keymap, "voice_search"), {}, {}, {}, {}}, &on);
  assert(on.size() == 1);  // start only
  assert(on[0].find("start") != std::string::npos);
}

// ---- the actually-shipped config/app.json
// --------------------------------------------------------

// The tests above use a hand-written copy of the keymap, which cannot catch app.json drifting away
// from the code (a rename landing in C++ but not in the config, or vice versa). This one reads the
// real file the Flatpak ships.
void test_shipped_app_json_keymap_is_current() {
  auto cfg = Config::load(DECKBACK_APP_JSON);
  assert(cfg.has_value());
  assert(!cfg->keymap.empty());

  // Voice ships DISABLED (V0 unverified), so `y` is genuinely a dead control today and the startup
  // warning about it is correct. If this flips, the V0 spike must have passed on hardware.
  assert(cfg->voice_enabled == false);
  assert(find_control_for_action(cfg->keymap, "voice_search") == BTN_Y);  // binding ready (Y=BTN_Y)

  // The layer sections ship EMPTY on purpose: no Leanback key is bound without an on-Deck spike. If
  // this ever fails, someone guessed a binding — that is the thing to re-examine, not this test.
  assert(cfg->keymap_player.empty());
  assert(cfg->keymap_osk.empty());
  assert(cfg->keymap_lt.empty());
  assert(cfg->keymap_rt.empty());

  // The whole shipped config resolves with nothing unmapped beyond the two documented controls.
  std::vector<std::string> layer_unmapped;
  build_keymaps(KeymapConfig{cfg->keymap, cfg->keymap_player, cfg->keymap_osk, cfg->keymap_lt,
                             cfg->keymap_rt},
                &layer_unmapped);
  assert(layer_unmapped.size() == 2);  // keymap.y=voice_search, keymap.start=player_menu

  for (const auto& [name, value] : cfg->keymap) {
    // Nothing we ship may use a deprecated alias — the compatibility path exists for *remote*
    // hot-swapped configs written against an older launcher, not for our own file.
    assert(deprecated_action_replacement(value).empty());
  }

  std::vector<std::string> unmapped;
  auto m = build_button_map(cfg->keymap, &unmapped);
  std::string k;
  assert(find_key(m, BTN_TL, &k) && k == "ArrowLeft");   // lb = scrub_back
  assert(find_key(m, BTN_TR, &k) && k == "ArrowRight");  // rb = scrub_fwd
  assert(find_key(m, BTN_SOUTH, &k) && k == "Enter");

  // Exactly the two documented unmapped controls: y (voice is not a key at all — input-ux §8.2) and
  // start (player_menu has no candidate key). A third would mean we broke a binding.
  assert(unmapped.size() == 2);
}

}  // namespace

int main() {
  test_binding_accepts_dom_keys_verbatim();
  test_binding_translates_semantic_actions();
  test_deprecated_seek_aliases_still_resolve();
  test_scan_keys_dispatchable_but_still_unbound();
  test_binding_refuses_to_guess_unknown_actions();

  test_direction_neutral_is_null();
  test_direction_vertical_beats_horizontal();
  test_direction_dpad_beats_stick();
  test_direction_returns_stable_pointers();

  test_button_map_from_shipped_keymap();
  test_button_map_reports_unknown_control_names();
  test_button_map_accepts_direct_dom_keys();
  test_button_map_tolerates_empty_keymap();

  test_trigger_hysteresis();

  test_skip_action_sign();
  test_build_skip_js_carries_the_signed_delta();

  test_parse_chord_valid();
  test_parse_chord_rejects_bad_input();

  test_config_parses_keymap_object();
  test_config_missing_keymap_is_empty_not_fatal();
  test_config_parses_touch_settings();
  test_config_touch_defaults();
  test_config_parses_right_stick();
  test_config_skip_seconds();

  test_fast_scroll_idle_inside_the_deadzone();
  test_fast_scroll_disabled_is_always_idle();
  test_fast_scroll_directions();
  test_fast_scroll_dominant_axis_wins_ties_go_vertical();
  test_fast_scroll_rate_is_analog_not_timed();
  test_fast_scroll_clamps_the_axis_minimum();
  test_fast_scroll_never_returns_a_busy_looping_interval();
  test_fast_scroll_degenerate_deadzone();

  test_chord_locks_immediately();
  test_chord_unlock_requires_a_deliberate_hold();
  test_chord_released_early_leaves_the_lock_engaged();
  test_chord_repeat_reports_do_not_restart_the_unlock_hold();
  test_chord_relock_after_unlock_needs_a_fresh_press();
  test_chord_zero_hold_unlocks_on_the_press();
  test_chord_negative_hold_is_clamped_not_undefined();
  test_chord_set_locked_reconciles_a_failed_grab();
  test_chord_never_acts_while_not_fully_held();

  test_find_voice_control();
  test_without_voice_control();
  test_voice_control_is_not_reported_unmapped_when_enabled();

  test_resolve_layer();
  test_layers_absent_is_context_free();
  test_context_layer_overrides_then_falls_through();
  test_modifier_layer_wins_and_absorbs();
  test_empty_modifier_layer_is_not_a_modifier();
  test_lt_wins_over_rt_when_both_held();
  test_build_layer_reports_unmapped_with_layer_label();
  test_empty_base_keeps_builtin_defaults();

  test_shipped_app_json_keymap_is_current();

  std::puts("input_test: all assertions passed");
  return 0;
}
