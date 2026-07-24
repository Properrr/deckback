// Pure helpers of the OSD controller (osdmenu.cpp): the Updates status line, the action-button set,
// and the verdict parser. The CDP/injection surface is exercised on-Deck (tests/deck/test_osd.py).

#include "osdmenu.hpp"

#include <cassert>
#include <cstdio>
#include <string>

#include "fake_cdp_server.hpp"

using namespace deckback;

void test_status_line() {
  assert(osd_status_line("0.0.4", "", false) == "Update status is not available.");
  assert(osd_status_line("0.0.4", "0.0.5", true) == "v0.0.5 is available. You have v0.0.4.");
  // An update is known but its version hasn't arrived yet (changelog fetch pending/failed/no-curl):
  // never claim "latest" while the Update-now buttons are showing.
  assert(osd_status_line("0.0.4", "", true) == "A newer version is available. You have v0.0.4.");
}

void test_button_is_redrawn_when_a_same_url_reload_wipes_it() {
  // Navigator only announces a transition into the app URL. A location.reload() keeps that state,
  // so the OSD has to notice the missing button itself instead of trusting button_shown_.
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
  assert(osd_update_buttons(false).empty());
  const auto b = osd_update_buttons(true);
  assert(b.size() == 2);
  assert(b[0].first == "update.confirm");
  assert(b[1].first == "update.ignore");
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

// capture <=> paint. A fresh controller's last_reconcile_ is the clock epoch, so the first tick()
// always runs the reconcile (the 750 ms throttle only rate-limits *repeated* checks) — one tick per
// controller is enough to assert one outcome. (OsdMenuController holds a mutex/atomic, so it can't
// be returned by value; each test builds its own in place.)

void test_reconcile_releases_capture_when_the_page_reloaded_the_menu_away() {
  // The bug: press Menu, the page reloads (suspend/resume, or a same-URL reload the navigator's
  // not-app->app transition never sees), the OSD paint is gone, but the launcher still captures
  // input — so keys pass through to Leanback behind an invisible/absent menu.
  testing::FakeServer srv;
  OsdMenuConfig cfg;
  cfg.cdp_port = srv.port();
  cfg.local_version = "0.0.4";
  OsdMenuController osd(cfg);
  assert(osd.open_menu());  // fake answers op:"open" with "ok"
  assert(osd.open());
  srv.set_osd_state("gone");  // JS context wiped by a full reload
  osd.tick(false);
  assert(!osd.open());  // capture released: input can never be trapped behind an absent menu
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
  // A Leanback body swap detaches the node momentarily; the shared keep-alive observer re-appends
  // it. The launcher must NOT tear the menu down for that, or every swap would drop the menu.
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

// Mid-video is exactly when Exit and the settings are wanted, and Menu has nothing else to do on
// the watch screen (`show_controls` is stripped from the keymap and resolves to no DOM key, so Menu
// was simply dead there). So the menu must survive a video appearing underneath it. Capture is
// modal, so that can only come from autoplay/up-next, never from the user driving Leanback behind
// the menu.
void test_menu_stays_open_over_playback() {
  testing::FakeServer srv;
  OsdMenuConfig cfg;
  cfg.cdp_port = srv.port();
  cfg.local_version = "0.0.4";
  OsdMenuController osd(cfg);
  assert(osd.open_menu());
  srv.set_osd_state("tab=settings;idx=-1");  // still painted
  osd.tick(true);                            // a video is up underneath
  assert(osd.open());                        // NOT torn down
}

// ...but the on-screen Settings button stays hidden on the watch screen, so it never sits over the
// video. Only the physical Menu button reaches the menu there.
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

// A on the Exit row must NOT act. It answers "hold" so the input layer can arm its deadline; only a
// completed hold fires. Anything that made this an Action would exit on a single stray press.
void test_hold_verdict_is_not_an_action() {
  const OsdVerdict h = parse_verdict("hold");
  assert(h.kind == OsdVerdict::Kind::Hold);
  assert(h.action.empty());
  assert(parse_verdict("hold").kind != OsdVerdict::Kind::Action);
  assert(parse_verdict("hold").kind != OsdVerdict::Kind::Close);
}

// The row is inert unless someone can actually act on it, so a build with no exit handler cannot
// paint an Exit control that does nothing when held.
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

// The hold duration the launcher counts and the one the fill animates are the SAME number, or the
// bar finishes at a different moment than the exit fires.
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

int main() {
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
  std::puts("osdmenu_test: all passed");
  return 0;
}
