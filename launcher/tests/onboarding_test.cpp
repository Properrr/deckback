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
      {"y", "voice_search"},
      {"lb", "scrub_back"},
      {"rb", "scrub_fwd"},
      {"lt", "scan_rewind"},
      {"rt", "scan_forward"},
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
}

void test_rows_omit_controls_that_do_nothing() {
  const auto rows = controls_overlay_rows(shipped());
  // L2/R2 -> scan_rewind/scan_forward resolve to no DOM key, so those buttons are dead. Printing
  // them on the card is the dead-button failure: it teaches the user the app is broken rather than
  // that the feature is absent.
  assert(!row_for(rows, "L2", nullptr));
  assert(!row_for(rows, "R2", nullptr));
  // Y -> voice_search, and voice ships disabled. Advertising "Hold to speak" on a build where the
  // mic button was never found is the same failure.
  assert(!row_for(rows, "Y", nullptr));

  // Same for any other keyless action: `player_menu` was Menu's binding until it was replaced,
  // precisely because it dispatched nothing.
  OverlayContext dead = shipped();
  dead.keymap = {{"start", "player_menu"}, {"a", "select"}};
  const auto dead_rows = controls_overlay_rows(dead);
  assert(!row_for(dead_rows, "Menu (☰)", nullptr));
  assert(row_for(dead_rows, "A", nullptr));
}

void test_voice_row_appears_only_when_voice_is_enabled() {
  OverlayContext c = shipped();
  assert(!any_action(controls_overlay_rows(c), "Hold to speak"));
  c.voice_enabled = true;
  std::string a;
  const auto rows = controls_overlay_rows(c);
  assert(row_for(rows, "Y", &a) && a == "Hold to speak");
}

void test_show_controls_action_is_listed_even_though_it_has_no_dom_key() {
  // `show_controls` is performed by the launcher, so resolve_binding() correctly reports no key for
  // it. If the card dropped every keyless action it would omit the row explaining how to reopen
  // itself — the one row a user who dismissed it by accident needs most.
  std::string a;
  const auto rows = controls_overlay_rows(shipped());
  assert(row_for(rows, "Menu (☰)", &a) && a == "These controls");
}

// The tests above use a hand-written copy of the keymap, which cannot catch app.json drifting away
// from the code. This one reads the real file the Flatpak ships and asserts the *invariant* rather
// than any particular binding: every row the card prints must name a control the hardware has and
// an action that actually happens. A row is a promise to the user.
void test_shipped_app_json_produces_no_dead_rows() {
  auto cfg = Config::load(DECKBACK_APP_JSON);
  assert(cfg.has_value());
  OverlayContext c{cfg->keymap, cfg->voice_enabled, cfg->right_stick_scroll,
                   cfg->touch_lock_enabled, cfg->touch_lock_chord};
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
  // Voice ships disabled, so the card must not promise voice search.
  assert(cfg->voice_enabled == false);
  assert(!any_action(rows, "Hold to speak"));
  // ...and the card must explain how to get itself back.
  assert(any_action(rows, "These controls"));
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

void test_touch_lock_row_names_the_configured_chord() {
  OverlayContext c = shipped();
  std::string a;
  auto rows = controls_overlay_rows(c);
  assert(row_for(rows, "L3 + R3", &a));
  assert(has(a, "Lock touchscreen"));
  assert(has(a, "hold to unlock"));  // the asymmetry is the surprising part, so it is on the card

  // A card that says L3+R3 while the config binds something else is a card that lies.
  c.touch_lock_chord = "select+start";
  rows = controls_overlay_rows(c);
  assert(!row_for(rows, "L3 + R3", nullptr));
  assert(row_for(rows, "SELECT + START", nullptr));

  // Disabled -> no row.
  c.touch_lock_enabled = false;
  assert(!row_for(controls_overlay_rows(c), "SELECT + START", nullptr));

  // An unparseable chord disables the feature in input.cpp, so it must not be advertised either.
  c.touch_lock_enabled = true;
  c.touch_lock_chord = "gibberish";
  for (const ControlRow& r : controls_overlay_rows(c)) assert(!has(r.action, "Lock touchscreen"));
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
  // hot-swappable text: `control` comes from control_label() today, but the chord row's control is
  // built straight from `touch_lock_chord`. ScriptParams escapes each once.
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

void test_dead_engine_leaves_it_hidden() {
  // If the card cannot be drawn, `visible()` must stay false — otherwise the input layer would go
  // modal and swallow every button press with nothing on screen to explain why.
  OnboardingController c("127.0.0.1", 8, shipped(), fresh_marker());
  assert(!c.show(false));
  assert(!c.visible());
}

}  // namespace

int main() {
  test_control_labels_are_what_is_printed_on_the_hardware();
  test_action_labels_pass_unknown_keys_through();

  test_rows_from_the_shipped_keymap();
  test_rows_omit_controls_that_do_nothing();
  test_voice_row_appears_only_when_voice_is_enabled();
  test_show_controls_action_is_listed_even_though_it_has_no_dom_key();
  test_shipped_app_json_produces_no_dead_rows();
  test_rows_follow_a_hot_swapped_keymap();
  test_dpad_entry_never_becomes_a_button_row();
  test_rows_preserve_keymap_order();
  test_launcher_owned_rows();
  test_touch_lock_row_names_the_configured_chord();

  test_overlay_js_renders_rows_and_escapes();
  test_overlay_hide_js_removes_it();

  test_marker_is_versioned_and_one_shot();
  test_marker_write_failure_is_survivable();

  test_show_first_run_only_once_ever();
  test_marker_is_written_on_show_not_on_dismiss();
  test_reopen_does_not_touch_the_marker();
  test_show_is_idempotent_and_hide_is_cheap();
  test_dead_engine_leaves_it_hidden();

  std::puts("onboarding_test: all assertions passed");
  return 0;
}
