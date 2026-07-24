// The first-run controls card (findings input-ux §17).
//
// The failure this file guards against is a card that *lies*. `config/app.json` is designed to be
// hot-swapped remotely (doc §6 R1), so a card with "View = Captions" baked into C++ would keep
// saying that after the config rebound View — and a controls card nobody can trust is worse than no
// card, because people stop reading it. Every row here must be derived from the same values the
// input layer resolves, and a control that dispatches nothing must not appear at all.
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
  // Users do not call these SELECT and START — Valve calls them View and Menu, and that is what the
  // glyphs on the shell show.
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
  // Deprecated aliases still resolve, so they must still print something truthful.
  assert(action_label("seek_back_10") == "Scrub back");
  // Launcher-performed seeks (LT/RT) have no DOM key but must read as a real feature, not vanish.
  assert(action_label("chapter_back") == "Previous chapter");
  assert(action_label("chapter_fwd") == "Next chapter");
  assert(action_label("skip_back") == "Skip back");
  // A raw DOM key from a hot-swapped config: print it rather than invent a phrase. The user who
  // wrote it knows what it means; we do not.
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
  // LT/RT are launcher-performed chapter seeks (no DOM key), so they must still be listed.
  assert(row_for(rows, "L2", &a) && a == "Previous chapter");
  assert(row_for(rows, "R2", &a) && a == "Next chapter");
}

void test_rows_omit_controls_that_do_nothing() {
  // A control bound to a keyless, non-launcher action (scan_rewind resolves to no DOM key and the
  // launcher cannot perform it) is dead. Printing it teaches the user the app is broken rather than
  // that the feature is absent.
  OverlayContext dead_trigger = shipped();
  dead_trigger.keymap = {{"lt", "scan_rewind"}, {"rt", "scan_forward"}, {"a", "select"}};
  const auto trig_rows = controls_overlay_rows(dead_trigger);
  assert(!row_for(trig_rows, "L2", nullptr));
  assert(!row_for(trig_rows, "R2", nullptr));

  const auto rows = controls_overlay_rows(shipped());
  // Y is unbound since the voice feature was removed, so it earns no row.
  assert(!row_for(rows, "Y", nullptr));

  // A keymap value on Menu cannot hide the fixed Settings entry point.
  OverlayContext dead = shipped();
  dead.keymap = {{"start", "player_menu"}, {"a", "select"}};
  const auto dead_rows = controls_overlay_rows(dead);
  std::string menu_action;
  assert(row_for(dead_rows, "Menu (☰)", &menu_action) && menu_action == "Settings");
  assert(row_for(dead_rows, "A", nullptr));
}

// The voice feature is gone. A hot-swapped config that still binds `voice_search` (written against
// an older launcher) must produce NO row: the action dispatches nothing, and a row is a promise.
void test_removed_voice_action_earns_no_row() {
  OverlayContext c = shipped();
  c.keymap.push_back({"y", "voice_search"});
  const auto rows = controls_overlay_rows(c);
  assert(!row_for(rows, "Y", nullptr));
  assert(!any_action(rows, "Hold to speak"));
  assert(!any_action(rows, "voice_search"));
}

void test_menu_is_a_fixed_settings_row_not_a_keymap_action() {
  // Menu always opens Settings now, even when a hot-swapped map removes/repurposes the legacy
  // show_controls action. The controls surface must teach that fixed route, not the retired card.
  std::string a;
  const auto rows = controls_overlay_rows(shipped());
  assert(row_for(rows, "Menu (☰)", &a) && a == "Settings");

  OverlayContext remapped = shipped();
  remapped.keymap = {{"start", "MediaPlayPause"}, {"a", "select"}};
  const auto remapped_rows = controls_overlay_rows(remapped);
  assert(row_for(remapped_rows, "Menu (☰)", &a) && a == "Settings");
  // It appears exactly once rather than once for the map and once for the fixed OSD entry point.
  size_t menu_rows = 0;
  for (const ControlRow& r : remapped_rows)
    if (r.control == "Menu (☰)") ++menu_rows;
  assert(menu_rows == 1);
}

// The tests above use a hand-written copy of the keymap, which cannot catch app.json drifting away
// from the code. This one reads the real file the Flatpak ships and asserts the *invariant* rather
// than any particular binding: every row the card prints must name a control the hardware has and
// an action that actually happens. A row is a promise to the user.
void test_shipped_app_json_produces_no_dead_rows() {
  auto cfg = Config::load(DECKBACK_APP_JSON);
  assert(cfg.has_value());
  OverlayContext c{cfg->keymap, cfg->right_stick_scroll};
  const auto rows = controls_overlay_rows(c);
  assert(!rows.empty());
  for (const ControlRow& r : rows) {
    assert(!r.control.empty());
    assert(!r.action.empty());
    // The card must never print a raw unmapped action name. If one of these appears, someone added
    // an action to app.json without teaching the launcher to perform it.
    assert(r.action != "player_menu");
    assert(r.action != "scan_rewind");
    assert(r.action != "scan_forward");
    assert(r.action != "navigate");
  }
  // The voice feature is gone, so the card must never promise voice search.
  assert(!any_action(rows, "Hold to speak"));
  // ...and the card must explain how to reach the fixed settings surface.
  assert(any_action(rows, "Settings"));
  // LT/RT ship bound to chapter seek — a launcher action with no DOM key. It must be listed, not
  // silently dropped as an unmapped binding (the "L2/R2 missing from the menu" regression).
  assert(any_action(rows, "Previous chapter"));
  assert(any_action(rows, "Next chapter"));
}

void test_rows_follow_a_hot_swapped_keymap() {
  // The whole point. Rebind A and the card must say so — nothing here is hardcoded.
  OverlayContext c;
  c.keymap = {{"a", "back"}, {"b", "select"}, {"select", "MediaTrackNext"}};
  std::string a;
  const auto rows = controls_overlay_rows(c);
  assert(row_for(rows, "A", &a) && a == "Back");
  assert(row_for(rows, "B", &a) && a == "Select");
  assert(row_for(rows, "View (⧉)", &a) && a == "MediaTrackNext");
}

void test_dpad_entry_never_becomes_a_button_row() {
  // The D-pad is described once, by the launcher, as "D-pad / Left stick". A `dpad` keymap entry
  // must never also print as if it were a face button — even if someone gives it a resolvable
  // value.
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
  // The D-pad and left stick are not keymap entries; the launcher owns them, so the card must add
  // the row itself or the most basic control on the device goes unmentioned.
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
  // A quote in a hot-swapped label would close the object literal; the card would then silently
  // never render, because a failed injection looks exactly like no injection. BOTH columns are
  // hot-swappable text, so ScriptParams escapes each once.
  assert(has(js, "say \\\"hi\\\""));
  assert(has(js, "quote\\\" in control"));
  assert(has(js, "back\\\\slash"));
  // The card body still removes any prior instance rather than stacking a second one forever.
  assert(has(js, "old.remove()"));
}

void test_overlay_hide_js_removes_it() {
  const std::string js = overlay_hide_js();
  assert(has(js, "__deckback_help"));
  assert(has(js, "remove()"));
}

// ---- first-run marker ---------------------------------------------------------------------------

void test_marker_is_versioned_and_one_shot() {
  const std::string dir = "/tmp/deckback_onboarding_test";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);

  const std::string marker = first_run_marker_path(dir);
  // Versioned, so a materially changed keymap can show the card once more instead of hiding behind
  // a marker written years earlier.
  assert(has(marker, "first_run_v1"));

  assert(first_run_pending(marker));
  assert(mark_first_run_done(marker));  // creates the directory too
  assert(!first_run_pending(marker));
  assert(mark_first_run_done(marker));  // idempotent
  assert(!first_run_pending(marker));

  std::filesystem::remove_all(dir, ec);
  assert(first_run_pending(marker));  // wiped state dir -> shown again, which is the safe direction
}

void test_marker_write_failure_is_survivable() {
  // A read-only state dir must not crash and must not be reported as success. The user then sees
  // the card again next launch, which is the milder of the two failures.
  assert(!mark_first_run_done("/proc/deckback_cannot_write_here/first_run_v1"));
}

// ---- OnboardingController over CDP
// ---------------------------------------------------------------

std::string fresh_marker() {
  const std::string dir = "/tmp/deckback_onboarding_ctl";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  return first_run_marker_path(dir);
}

void test_show_first_run_only_once_ever() {
  testing::FakeServer server;
  const std::string marker = fresh_marker();

  // Each controller is scoped: FakeServer serves one connection at a time, and a live
  // DevToolsClient holds its WebSocket open. Two overlapping controllers would starve the second of
  // a connection — which is a property of the fake, not of content_shell (M114 allows concurrent
  // CDP clients).
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

    // ...but the user can always reopen it deliberately.
    assert(c2.show(/*first_run_only=*/false));
    assert(c2.visible());
  }
}

void test_marker_is_written_on_show_not_on_dismiss() {
  // Otherwise a crash (or a Steam "Close game") while the card is up means it reappears next
  // launch, forever, for anyone whose app dies during onboarding.
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
  c.hide();  // already hidden: must not spend a CDP round trip on every button press
  assert(server.take_requests().empty());
}

// The card is drawn into Leanback's document, and Leanback reloads it out from under us with no
// signal we can observe. Before PageOverlay, `visible()` stayed true forever after that: the input
// layer went on swallowing every event and pinning the D-pad, and the user's next button press was
// spent dismissing a card that had already vanished.
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
  // The other half: a health check that clears the flag on a live, present card would dismiss the
  // card on its own a beat after it appeared.
  testing::FakeServer server;
  OnboardingController c("127.0.0.1", server.port(), shipped(), fresh_marker());
  assert(c.show(false));
  c.tick();
  assert(c.visible());
}

void test_dead_engine_leaves_it_hidden() {
  // If the card cannot be drawn, `visible()` must stay false — otherwise the input layer would go
  // modal and swallow every button press with nothing on screen to explain why.
  OnboardingController c("127.0.0.1", 8, shipped(), fresh_marker());
  assert(!c.show(false));
  assert(!c.visible());
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
  test_dead_engine_leaves_it_hidden();

  std::puts("onboarding_test: all assertions passed");
  return 0;
}
