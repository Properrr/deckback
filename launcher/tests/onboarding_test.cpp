// The first-run controls card: every row derives from the live keymap, and a control that
// dispatches nothing gets no row.
#include "onboarding.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>

#include "config.hpp"
#include "fake_cdp_server.hpp"
#include "harness.hpp"

using namespace deckback;

namespace {

bool has(const std::string& s, const std::string& n) { return s.find(n) != std::string::npos; }

bool row_for(const std::vector<ControlRow>& rows, const std::string& control, std::string* action) {
  for (const ControlRow& r : rows)
    if (r.control == control) {
      if (action) *action = r.action;
      return true;
    }
  return false;
}

bool any_action(const std::vector<ControlRow>& rows, const std::string& action) {
  for (const ControlRow& r : rows)
    if (r.action == action) return true;
  return false;
}

// The keymap as config/app.json actually ships it.
OverlayContext shipped() {
  OverlayContext c;
  c.keymap = {
      {"dpad", "navigate"},
      {"a", "select"},
      {"b", "back"},
      {"x", "playpause"},
      {"lb", "scrub_back"},
      {"rb", "scrub_fwd"},
      {"lt", "chapter_back"},
      {"rt", "chapter_fwd"},
      {"start", "show_controls"},
      {"select", "toggle_captions"},
  };
  return c;
}

// ---- labels -------------------------------------------------------------------------------------

void test_control_labels_are_what_is_printed_on_the_hardware() {
  // SELECT and START are labelled View and Menu, as printed on the Deck.
  assert(control_label("select") == "View (⧉)");
  assert(control_label("start") == "Menu (☰)");
  assert(control_label("lb") == "L1");
  assert(control_label("rt") == "R2");
  assert(control_label("a") == "A");
  assert(control_label("nope").empty());
}

void test_action_labels_pass_unknown_keys_through() {
  assert(action_label("playpause") == "Play / pause");
  assert(action_label("toggle_captions") == "Captions");
  // Each alias the binding predicates accept prints the same phrase, not its raw config name.
  for (const char* a : {"toggle_captions", "captions"}) assert(action_label(a) == "Captions");
  for (const char* a : {"toggle_gestures", "touch_gestures", "toggle_touch"})
    assert(action_label(a) == "Touch gestures on / off");
  assert(action_label("seek_back_10") == "Scrub back");
  // Launcher-performed seeks have no DOM key but still get a label.
  assert(action_label("chapter_back") == "Previous chapter");
  assert(action_label("chapter_fwd") == "Next chapter");
  assert(action_label("skip_back") == "Skip back");
  // A raw DOM key prints verbatim.
  assert(action_label("MediaTrackNext") == "MediaTrackNext");
}

// ---- rows ---------------------------------------------------------------------------------------

void test_rows_from_the_shipped_keymap() {
  const auto rows = controls_overlay_rows(shipped());
  std::string a;
  assert(row_for(rows, "A", &a) && a == "Select");
  assert(row_for(rows, "B", &a) && a == "Back");
  assert(row_for(rows, "X", &a) && a == "Play / pause");
  assert(row_for(rows, "View (⧉)", &a) && a == "Captions");
  assert(row_for(rows, "L1", &a) && a == "Scrub back");
  assert(row_for(rows, "R1", &a) && a == "Scrub forward");
  // LT/RT are launcher-performed chapter seeks (no DOM key) and are still listed.
  assert(row_for(rows, "L2", &a) && a == "Previous chapter");
  assert(row_for(rows, "R2", &a) && a == "Next chapter");
}

void test_rows_omit_controls_that_do_nothing() {
  // A control bound to an action with no DOM key and no launcher handler gets no row.
  OverlayContext dead_trigger = shipped();
  dead_trigger.keymap = {{"lt", "scan_rewind"}, {"rt", "scan_forward"}, {"a", "select"}};
  const auto trig_rows = controls_overlay_rows(dead_trigger);
  assert(!row_for(trig_rows, "L2", nullptr));
  assert(!row_for(trig_rows, "R2", nullptr));

  const auto rows = controls_overlay_rows(shipped());
  assert(!row_for(rows, "Y", nullptr));

  // A keymap value on Menu cannot hide the fixed Settings entry point.
  OverlayContext dead = shipped();
  dead.keymap = {{"start", "player_menu"}, {"a", "select"}};
  const auto dead_rows = controls_overlay_rows(dead);
  std::string menu_action;
  assert(row_for(dead_rows, "Menu (☰)", &menu_action) && menu_action == "Settings");
  assert(row_for(dead_rows, "A", nullptr));
}

// A config that binds `voice_search` (which dispatches nothing) produces no row.
void test_removed_voice_action_earns_no_row() {
  OverlayContext c = shipped();
  c.keymap.push_back({"y", "voice_search"});
  const auto rows = controls_overlay_rows(c);
  assert(!row_for(rows, "Y", nullptr));
  assert(!any_action(rows, "Hold to speak"));
  assert(!any_action(rows, "voice_search"));
}

void test_menu_is_a_fixed_settings_row_not_a_keymap_action() {
  // Menu always opens Settings, whatever the keymap binds to start.
  std::string a;
  const auto rows = controls_overlay_rows(shipped());
  assert(row_for(rows, "Menu (☰)", &a) && a == "Settings");

  OverlayContext remapped = shipped();
  remapped.keymap = {{"start", "MediaPlayPause"}, {"a", "select"}};
  const auto remapped_rows = controls_overlay_rows(remapped);
  assert(row_for(remapped_rows, "Menu (☰)", &a) && a == "Settings");
  // It appears exactly once.
  size_t menu_rows = 0;
  for (const ControlRow& r : remapped_rows)
    if (r.control == "Menu (☰)") ++menu_rows;
  assert(menu_rows == 1);
}

// Every row from the real app.json names a real control and an action that actually happens.
void test_shipped_app_json_produces_no_dead_rows() {
  auto cfg = Config::load(DECKBACK_APP_JSON);
  assert(cfg.has_value());
  OverlayContext c{cfg->keymap, cfg->right_stick_scroll};
  const auto rows = controls_overlay_rows(c);
  assert(!rows.empty());
  for (const ControlRow& r : rows) {
    assert(!r.control.empty());
    assert(!r.action.empty());
    // No raw unmapped action names.
    assert(r.action != "player_menu");
    assert(r.action != "scan_rewind");
    assert(r.action != "scan_forward");
    assert(r.action != "navigate");
  }
  assert(!any_action(rows, "Hold to speak"));
  assert(any_action(rows, "Settings"));
  // LT/RT chapter seek is a launcher action with no DOM key, and is listed.
  assert(any_action(rows, "Previous chapter"));
  assert(any_action(rows, "Next chapter"));
}

void test_rows_follow_a_hot_swapped_keymap() {
  // Rebinding A changes its row.
  OverlayContext c;
  c.keymap = {{"a", "back"}, {"b", "select"}, {"select", "MediaTrackNext"}};
  std::string a;
  const auto rows = controls_overlay_rows(c);
  assert(row_for(rows, "A", &a) && a == "Back");
  assert(row_for(rows, "B", &a) && a == "Select");
  assert(row_for(rows, "View (⧉)", &a) && a == "MediaTrackNext");
}

void test_dpad_entry_never_becomes_a_button_row() {
  // The launcher describes the D-pad once; a `dpad` keymap entry never prints as a button row.
  OverlayContext c;
  c.keymap = {{"dpad", "select"}};
  const auto rows = controls_overlay_rows(c);
  assert(!row_for(rows, "D-pad", nullptr));
  assert(row_for(rows, "D-pad / Left stick", nullptr));
}

void test_rows_preserve_keymap_order() {
  OverlayContext c;
  c.keymap = {{"select", "toggle_captions"}, {"a", "select"}};
  const auto rows = controls_overlay_rows(c);
  assert(rows.size() >= 2);
  assert(rows[0].control == "View (⧉)");
  assert(rows[1].control == "A");
}

void test_launcher_owned_rows() {
  OverlayContext c = shipped();
  std::string a;
  // The D-pad/left stick row is added by the launcher, not the keymap.
  auto rows = controls_overlay_rows(c);
  assert(row_for(rows, "D-pad / Left stick", &a) && a == "Move focus");

  // The right stick row tracks the feature flag.
  assert(row_for(rows, "Right stick", &a) && a == "Fast scroll");
  c.right_stick_scroll = false;
  assert(!row_for(controls_overlay_rows(c), "Right stick", nullptr));
}

// ---- overlay_js ---------------------------------------------------------------------------------

void test_overlay_js_renders_rows_and_escapes() {
  const std::vector<ControlRow> rows = {
      {"A", "Select"}, {"B", "say \"hi\""}, {"quote\" in control", "back\\slash"}};
  const std::string js = overlay_js(rows, "Controls", "Press any button");
  assert(has(js, "__deckback_help"));
  assert(has(js, "document.documentElement.appendChild"));  // Leanback replaces body on navigation
  // Rows are passed as structured [control, action] data; the page builds the <td>s at runtime.
  assert(has(js, "[\"A\",\"Select\"]"));
  assert(has(js, "Controls"));          // the title param
  assert(has(js, "Press any button"));  // the footer param
  // Both columns are escaped exactly once.
  assert(has(js, "say \\\"hi\\\""));
  assert(has(js, "quote\\\" in control"));
  assert(has(js, "back\\\\slash"));
  // A prior instance is removed rather than stacked.
  assert(has(js, "old.remove()"));
}

void test_overlay_hide_js_removes_it() {
  const std::string js = overlay_hide_js();
  assert(has(js, "__deckback_help"));
  assert(has(js, "remove()"));
}

// ---- first-run marker ---------------------------------------------------------------------------

void test_marker_is_versioned_and_one_shot() {
  const std::string dir = (deckback_test_tmpdir() / "deckback_onboarding_test").string();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);

  const std::string marker = first_run_marker_path(dir);
  // Versioned, so a changed keymap can show the card once more.
  assert(has(marker, "first_run_v1"));

  assert(first_run_pending(marker));
  assert(mark_first_run_done(marker));  // creates the directory too
  assert(!first_run_pending(marker));
  assert(mark_first_run_done(marker));  // idempotent
  assert(!first_run_pending(marker));

  std::filesystem::remove_all(dir, ec);
  assert(first_run_pending(marker));  // wiped state dir -> shown again
}

void test_marker_write_failure_is_survivable() {
  // A read-only state dir does not crash and is not reported as success.
  assert(!mark_first_run_done("/proc/deckback_cannot_write_here/first_run_v1"));
}

// ---- OnboardingController over CDP --------------------------------------------------------------

std::string fresh_marker() {
  const std::string dir = (deckback_test_tmpdir() / "deckback_onboarding_ctl").string();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  return first_run_marker_path(dir);
}

void test_show_first_run_only_once_ever() {
  testing::FakeServer server;
  const std::string marker = fresh_marker();

  // Each controller is scoped: FakeServer serves one connection at a time.
  {
    OnboardingController c("127.0.0.1", server.port(), shipped(), marker);
    assert(!c.visible());
    assert(c.show(/*first_run_only=*/true));
    assert(c.visible());
    c.hide();
    assert(!c.visible());
  }
  {
    // Second launch: the marker is on disk, so the card stays away.
    OnboardingController c2("127.0.0.1", server.port(), shipped(), marker);
    assert(!c2.show(/*first_run_only=*/true));
    assert(!c2.visible());

    // A deliberate reopen still works.
    assert(c2.show(/*first_run_only=*/false));
    assert(c2.visible());
  }
}

void test_marker_is_written_on_show_not_on_dismiss() {
  // So a crash while the card is up does not show it again next launch.
  testing::FakeServer server;
  const std::string marker = fresh_marker();
  OnboardingController c("127.0.0.1", server.port(), shipped(), marker);
  assert(c.show(true));
  assert(!first_run_pending(marker));  // already recorded, before any dismiss
}

void test_reopen_does_not_touch_the_marker() {
  testing::FakeServer server;
  const std::string marker = fresh_marker();
  OnboardingController c("127.0.0.1", server.port(), shipped(), marker);
  assert(c.show(/*first_run_only=*/false));
  assert(first_run_pending(marker));  // a deliberate reopen is not the first run
}

void test_show_is_idempotent_and_hide_is_cheap() {
  testing::FakeServer server;
  OnboardingController c("127.0.0.1", server.port(), shipped(), fresh_marker());
  assert(c.show(false));
  server.take_requests();
  assert(!c.show(false));  // already up: no second injection
  assert(server.take_requests().empty());

  c.hide();
  server.take_requests();
  c.hide();  // already hidden: no CDP round trip
  assert(server.take_requests().empty());
}

// When a page reload wipes the card, tick() notices and releases input capture.
void test_tick_releases_capture_when_the_page_ate_the_card() {
  testing::FakeServer server;
  OnboardingController c("127.0.0.1", server.port(), shipped(), fresh_marker());
  assert(c.show(false));
  assert(c.visible());

  server.set_overlay_present(false);  // a reload wiped documentElement
  c.tick();
  assert(!c.visible());  // capture released without the user having to press anything
}

void test_tick_keeps_the_card_while_it_is_still_painted() {
  // tick() leaves a card that is still present alone.
  testing::FakeServer server;
  OnboardingController c("127.0.0.1", server.port(), shipped(), fresh_marker());
  assert(c.show(false));
  c.tick();
  assert(c.visible());
}

// A hide that never reached the page leaves the card visible (and modal).
void test_a_failed_hide_keeps_the_card() {
  testing::FakeServer server;
  OnboardingController c("127.0.0.1", server.port(), shipped(), fresh_marker());
  assert(c.show(false));
  server.close_after_frames(1);  // the session drops the next request unanswered
  c.hide();
  assert(c.visible());
}

void test_dead_engine_leaves_it_hidden() {
  // If the card cannot be drawn, `visible()` stays false so input is not captured.
  OnboardingController c("127.0.0.1", 8, shipped(), fresh_marker());
  assert(!c.show(false));
  assert(!c.visible());
}

// Touch gesture rows appear exactly when the gesture router is enabled.
void test_touch_rows_track_the_router() {
  OverlayContext off{{{"a", "select"}}, false, false};
  for (const ControlRow& r : controls_overlay_rows(off)) {
    assert(r.control.find("Swipe") == std::string::npos);
    assert(r.control.find("Tap") == std::string::npos);
  }

  OverlayContext on{{{"a", "select"}}, false, true};
  const auto rows = controls_overlay_rows(on);
  bool swipe = false, tap = false, dtap = false, hold = false, edge = false;
  for (const ControlRow& r : rows) {
    if (r.control == "Swipe") swipe = true;
    if (r.control == "Tap") tap = true;
    if (r.control.rfind("Double-tap", 0) == 0) dtap = true;
    if (r.control.rfind("Hold", 0) == 0) hold = true;
    if (r.control.find("left edge") != std::string::npos) edge = true;
  }
  assert(swipe && tap && dtap && hold && edge);
}

// The gesture toggle (a launcher action with no DOM key) is listed.
void test_the_gesture_toggle_is_listed() {
  OverlayContext ctx{{{"y", "toggle_gestures"}}, false, true};
  bool found = false;
  for (const ControlRow& r : controls_overlay_rows(ctx))
    if (r.action.find("Touch gestures") != std::string::npos) found = true;
  assert(found);
}

}  // namespace

DECKBACK_TEST_MAIN(onboarding) {
  test_control_labels_are_what_is_printed_on_the_hardware();
  test_action_labels_pass_unknown_keys_through();

  test_rows_from_the_shipped_keymap();
  test_rows_omit_controls_that_do_nothing();
  test_removed_voice_action_earns_no_row();
  test_menu_is_a_fixed_settings_row_not_a_keymap_action();
  test_shipped_app_json_produces_no_dead_rows();
  test_rows_follow_a_hot_swapped_keymap();
  test_dpad_entry_never_becomes_a_button_row();
  test_rows_preserve_keymap_order();
  test_launcher_owned_rows();

  test_overlay_js_renders_rows_and_escapes();
  test_overlay_hide_js_removes_it();

  test_marker_is_versioned_and_one_shot();
  test_marker_write_failure_is_survivable();

  test_show_first_run_only_once_ever();
  test_marker_is_written_on_show_not_on_dismiss();
  test_reopen_does_not_touch_the_marker();
  test_show_is_idempotent_and_hide_is_cheap();
  test_tick_releases_capture_when_the_page_ate_the_card();
  test_tick_keeps_the_card_while_it_is_still_painted();
  test_a_failed_hide_keeps_the_card();
  test_dead_engine_leaves_it_hidden();

  test_touch_rows_track_the_router();
  test_the_gesture_toggle_is_listed();
  std::puts("onboarding_test: all assertions passed");
  return 0;
}
