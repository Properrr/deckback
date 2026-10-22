// The OSD controller: status line, update buttons, verdict parsing, and capture/paint reconciling
// against a fake CDP server.

#include "osdmenu.hpp"

#include <cassert>
#include <cstdio>
#include <string>

#include "fake_cdp_server.hpp"
#include "harness.hpp"

using namespace deckback;

void test_status_line() {
  assert(osd_status_line("0.0.4", "", false) == "Update status is not available.");
  assert(osd_status_line("0.0.4", "0.0.5", true) == "v0.0.5 is available. You have v0.0.4.");
  // An update is known but its version is not.
  assert(osd_status_line("0.0.4", "", true) == "A newer version is available. You have v0.0.4.");
}

void test_button_is_redrawn_when_a_same_url_reload_wipes_it() {
  // A same-URL reload is not announced by the navigator, so tick() probes for the button.
  testing::FakeServer srv;
  OsdMenuConfig cfg;
  cfg.cdp_port = srv.port();
  OsdMenuController osd(cfg);
  osd.tick(false);  // initial Settings button draw
  srv.take_requests();

  srv.set_osd_button_present(false);  // document was replaced by a same-URL reload
  osd.tick(false);
  const auto requests = srv.take_requests();
  bool redrawn = false;
  for (const std::string& request : requests)
    if (request.find("__deckback_settings_btn") != std::string::npos &&
        request.find("osd-button-state") == std::string::npos)
      redrawn = true;
  assert(redrawn);
}

void test_update_buttons() {
  // Self-update off: no buttons.
  assert(osd_update_buttons(false, false, false).empty());

  const auto b = osd_update_buttons(true, false, false);
  assert(b.size() == 2);
  assert(b[0].first == "update.confirm");
  assert(b[1].first == "update.ignore");

  // No announced update but a live updater: the manual check.
  const auto c = osd_update_buttons(false, true, false);
  assert(c.size() == 1);
  assert(c[0].first == "update.check");

  // An announced update replaces the check.
  const auto d = osd_update_buttons(true, true, false);
  assert(d.size() == 2);
  assert(d[0].first == "update.confirm");

  // A staged update offers only Quit, even if has_update is still set.
  const auto e = osd_update_buttons(true, true, true);
  assert(e.size() == 1);
  assert(e[0].first == "update.quit");
}

void test_parse_verdict() {
  assert(parse_verdict("consumed").kind == OsdVerdict::Kind::Consumed);
  assert(parse_verdict("close").kind == OsdVerdict::Kind::Close);
  assert(parse_verdict("gone").kind == OsdVerdict::Kind::Gone);
  assert(parse_verdict("").kind ==
         OsdVerdict::Kind::Gone);  // empty eval result == absent component

  const OsdVerdict a = parse_verdict("action:update.confirm");
  assert(a.kind == OsdVerdict::Kind::Action);
  assert(a.action == "update.confirm");

  const OsdVerdict ap = parse_verdict("apply:cc.type=auto_first");
  assert(ap.kind == OsdVerdict::Kind::Apply);
  assert(ap.action == "cc.type=auto_first");

  // An unknown non-action token is treated as consumed, never as an action.
  assert(parse_verdict("whatever").kind == OsdVerdict::Kind::Consumed);
  assert(parse_verdict("action:").kind == OsdVerdict::Kind::Action);
  assert(parse_verdict("action:").action.empty());
}

// Capture follows paint. A fresh controller's first tick() always reconciles (the 750 ms throttle
// only limits repeats), so one tick per controller asserts one outcome.

void test_reconcile_releases_capture_when_the_page_reloaded_the_menu_away() {
  // A reload wiped the menu: capture is released.
  testing::FakeServer srv;
  OsdMenuConfig cfg;
  cfg.cdp_port = srv.port();
  cfg.local_version = "0.0.4";
  OsdMenuController osd(cfg);
  assert(osd.open_menu());  // fake answers op:"open" with "ok"
  assert(osd.open());
  srv.set_osd_state("gone");  // JS context wiped by a full reload
  osd.tick(false);
  assert(!osd.open());
}

void test_reconcile_keeps_capture_while_painted() {
  testing::FakeServer srv;
  OsdMenuConfig cfg;
  cfg.cdp_port = srv.port();
  cfg.local_version = "0.0.4";
  OsdMenuController osd(cfg);
  assert(osd.open_menu());
  srv.set_osd_state("tab=settings;idx=-1");  // still on screen
  osd.tick(false);
  assert(osd.open());
}

void test_reconcile_ignores_a_detached_node() {
  // A momentarily detached node (re-appended by the keep-alive observer) keeps the menu open.
  testing::FakeServer srv;
  OsdMenuConfig cfg;
  cfg.cdp_port = srv.port();
  cfg.local_version = "0.0.4";
  OsdMenuController osd(cfg);
  assert(osd.open_menu());
  srv.set_osd_state("detached");
  osd.tick(false);
  assert(osd.open());
}

// The menu stays open when a video starts underneath it (autoplay/up-next).
void test_menu_stays_open_over_playback() {
  testing::FakeServer srv;
  OsdMenuConfig cfg;
  cfg.cdp_port = srv.port();
  cfg.local_version = "0.0.4";
  OsdMenuController osd(cfg);
  assert(osd.open_menu());
  srv.set_osd_state("tab=settings;idx=-1");  // still painted
  osd.tick(true);                            // a video is up underneath
  assert(osd.open());
}

// The on-screen Settings button is not drawn over playback.
void test_settings_button_is_not_drawn_over_playback() {
  testing::FakeServer srv;
  OsdMenuConfig cfg;
  cfg.cdp_port = srv.port();
  OsdMenuController osd(cfg);
  srv.take_requests();
  osd.tick(true);  // on watch
  for (const std::string& request : srv.take_requests())
    assert(request.find("__deckback_settings_btn") == std::string::npos ||
           request.find("osd-button-state") != std::string::npos);
}

// A on the Exit row answers "hold" (arm the deadline), never an action.
void test_hold_verdict_is_not_an_action() {
  const OsdVerdict h = parse_verdict("hold");
  assert(h.kind == OsdVerdict::Kind::Hold);
  assert(h.action.empty());
  assert(parse_verdict("hold").kind != OsdVerdict::Kind::Action);
  assert(parse_verdict("hold").kind != OsdVerdict::Kind::Close);
}

// Without an exit handler the Exit row is painted disabled.
void test_exit_row_is_disabled_without_a_handler() {
  testing::FakeServer srv;
  OsdMenuConfig cfg;
  cfg.cdp_port = srv.port();
  OsdMenuController osd(cfg);  // no on_exit
  srv.take_requests();
  assert(osd.open_menu());
  bool saw_disabled = false;
  for (const std::string& request : srv.take_requests())
    if (request.find("\\\"exit_enabled\\\":false") != std::string::npos ||
        request.find("\"exit_enabled\":false") != std::string::npos)
      saw_disabled = true;
  assert(saw_disabled);
}

// The launcher's hold duration is passed to the page's fill animation.
void test_hold_ms_is_shared_with_the_page() {
  testing::FakeServer srv;
  OsdMenuConfig cfg;
  cfg.cdp_port = srv.port();
  cfg.exit_hold_ms = 750;
  cfg.on_exit = [] {};
  OsdMenuController osd(cfg);
  assert(osd.exit_hold_ms() == 750);
  srv.take_requests();
  assert(osd.open_menu());
  bool saw = false;
  for (const std::string& request : srv.take_requests())
    if (request.find("hold_ms") != std::string::npos && request.find("750") != std::string::npos)
      saw = true;
  assert(saw);
}

// A command the page answers with "gone" (its JS context was wiped) releases capture at once.
void test_gone_command_releases_capture() {
  testing::FakeServer srv;
  OsdMenuConfig cfg;
  cfg.cdp_port = srv.port();
  OsdMenuController osd(cfg);
  assert(osd.open_menu());
  srv.set_osd_cmd_reply("gone");
  assert(parse_verdict(osd.exec("down")).kind == OsdVerdict::Kind::Gone);
  assert(!osd.open());
}

// An unanswered command keeps capture; the reconcile tick asks the page instead.
void test_unanswered_command_keeps_capture() {
  testing::FakeServer srv;
  OsdMenuConfig cfg;
  cfg.cdp_port = srv.port();
  OsdMenuController osd(cfg);
  assert(osd.open_menu());
  srv.set_osd_cmd_reply("gone");  // would release capture, had the frame been answered
  srv.close_after_frames(static_cast<int>(srv.take_requests().size()));  // drop the next frame
  const std::string v = osd.exec("down");
  assert(v == kOsdUnreachable);
  assert(parse_verdict(v).kind == OsdVerdict::Kind::Unreachable);
  assert(osd.open());

  srv.close_after_frames(0);
  srv.set_osd_state("gone");  // the page now says the menu really is gone
  osd.tick(false);
  assert(!osd.open());
}

DECKBACK_TEST_MAIN(osdmenu) {
  test_status_line();
  test_button_is_redrawn_when_a_same_url_reload_wipes_it();
  test_update_buttons();
  test_parse_verdict();
  test_reconcile_releases_capture_when_the_page_reloaded_the_menu_away();
  test_reconcile_keeps_capture_while_painted();
  test_reconcile_ignores_a_detached_node();
  test_menu_stays_open_over_playback();
  test_settings_button_is_not_drawn_over_playback();
  test_hold_verdict_is_not_an_action();
  test_exit_row_is_disabled_without_a_handler();
  test_hold_ms_is_shared_with_the_page();
  test_gone_command_releases_capture();
  test_unanswered_command_keeps_capture();
  std::puts("osdmenu_test: all passed");
  return 0;
}
