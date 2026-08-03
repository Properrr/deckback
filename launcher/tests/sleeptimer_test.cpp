// L0 coverage of the sleep timer's decision logic (TASKS P12.6).
//
// The timer's whole job is to fire ONCE, at the right moment, and then get out of the way. Every
// failure mode worth having a test for is an edge that only shows up at a boundary: firing twice
// (which would fight a user who presses play again), warning about a countdown too short for the
// warning to mean anything, or a countdown that quietly restarts across a suspend instead of being
// over. None of those need an engine, a Deck, or a clock — `tick()` takes the time as an argument
// precisely so they are all reachable here.
//
// What is NOT covered here, and cannot be: that pausing the video actually lets the Deck sleep.
// That is a property of logind, gamescope and the host nudge helper, and it is asserted from
// outside the app in tests/deck/test_sleep.py.
#include "sleeptimer.hpp"

#include <cassert>
#include <cstdio>
#include <string>

#include "harness.hpp"

namespace deckback {
namespace {

constexpr long kMin = 60'000;

SleepTimer make(int warn_seconds = 60) { return SleepTimer({5, 15, 30}, warn_seconds); }

void test_a_fresh_timer_is_off() {
  SleepTimer t = make();
  assert(!t.armed());
  assert(t.selected_minutes() == 0);
  assert(t.status_line(0) == "Off");
  assert(t.remaining_ms(0) == 0);
  assert(t.tick(999'999) == SleepTimer::Tick::None);
}

void test_it_fires_once_at_the_deadline_and_disarms() {
  SleepTimer t = make(/*warn_seconds=*/0);
  t.arm(5, 1000);
  assert(t.armed());
  assert(t.tick(1000 + 5 * kMin - 1) == SleepTimer::Tick::None);
  assert(t.tick(1000 + 5 * kMin) == SleepTimer::Tick::Fire);
  // Disarmed by the fire: a timer that stayed armed would re-pause the video on every following
  // tick, so pressing play would be undone a second later.
  assert(!t.armed());
  assert(t.tick(1000 + 6 * kMin) == SleepTimer::Tick::None);
  assert(t.status_line(1000 + 6 * kMin) == "Off");
}

void test_a_countdown_that_elapsed_during_suspend_is_over_not_restarted() {
  // tick() is fed CLOCK_BOOTTIME, which keeps counting while the Deck is asleep. A four-hour sleep
  // in the middle of a 30-minute countdown must come back expired.
  SleepTimer t = make();
  t.arm(30, 0);
  assert(t.tick(4 * 60 * kMin) == SleepTimer::Tick::Fire);
}

void test_the_warning_fires_once_inside_the_lead() {
  SleepTimer t = make(/*warn_seconds=*/60);
  t.arm(15, 0);
  assert(t.tick(13 * kMin) == SleepTimer::Tick::None);
  assert(t.tick(14 * kMin) == SleepTimer::Tick::Warn);
  assert(t.tick(14 * kMin + 5000) == SleepTimer::Tick::None);  // only once
  assert(t.tick(15 * kMin) == SleepTimer::Tick::Fire);
}

void test_no_warning_for_a_duration_no_longer_than_the_lead() {
  // A 30 s ladder entry (what a hot-swapped test config uses) with a 60 s lead would warn in the
  // same breath as the arming, which tells the user nothing they did not just choose.
  SleepTimer t = SleepTimer({0.5}, /*warn_seconds=*/60);
  t.arm(0.5, 0);
  assert(t.tick(1000) == SleepTimer::Tick::None);
  assert(t.tick(29'000) == SleepTimer::Tick::None);
  assert(t.tick(30'000) == SleepTimer::Tick::Fire);
}

void test_warn_disabled_is_honoured() {
  SleepTimer t = make(/*warn_seconds=*/0);
  t.arm(5, 0);
  assert(t.tick(5 * kMin - 1) == SleepTimer::Tick::None);
  assert(t.tick(5 * kMin) == SleepTimer::Tick::Fire);
}

void test_the_lead_is_readable_so_the_toast_can_state_it() {
  // The warning toast says how long is left, and it must get that from the configured lead rather
  // than hard-coding "a minute" — which a hot-swapped sleep_timer_warn_seconds would turn into a
  // lie with nothing failing.
  assert(make(60).warn_seconds() == 60);
  assert(make(300).warn_seconds() == 300);
  assert(format_remaining(make(300).warn_seconds() * 1000L) == "5 min");
  // A negative lead is a config typo, not "warn in the past".
  assert(make(-5).warn_seconds() == 0);
}

void test_rearming_resets_both_edges() {
  SleepTimer t = make(/*warn_seconds=*/60);
  t.arm(15, 0);
  assert(t.tick(14 * kMin) == SleepTimer::Tick::Warn);
  t.arm(30, 14 * kMin);  // changed my mind, longer
  assert(t.tick(14 * kMin + 1000) == SleepTimer::Tick::None);
  assert(t.tick(43 * kMin) == SleepTimer::Tick::Warn);  // warned again, not swallowed
  assert(t.tick(44 * kMin) == SleepTimer::Tick::Fire);
}

void test_remaining_and_status_count_down() {
  SleepTimer t = make();
  t.arm(15, 0);
  assert(t.remaining_ms(0) == 15 * kMin);
  assert(t.remaining_ms(5 * kMin) == 10 * kMin);
  // Never negative, even if a tick lands after the deadline but before the fire is processed.
  assert(t.remaining_ms(99 * kMin) == 0);
  assert(t.status_line(0) == "Stops in 15 min");
  assert(t.status_line(14 * kMin + 30'000) == "Stops in 30 s");
}

void test_remaining_rounds_up_so_a_fresh_timer_shows_what_was_picked() {
  // Truncating would print "14 min" the instant a 15-minute timer is armed, which reads as the
  // launcher having ignored the choice.
  assert(format_remaining(15 * kMin) == "15 min");
  assert(format_remaining(15 * kMin - 1) == "15 min");
  assert(format_remaining(14 * kMin + 1) == "15 min");
  // Under a minute it counts seconds: "1 min" frozen for 59 s reads as a stuck timer.
  assert(format_remaining(59'000) == "59 s");
  assert(format_remaining(1) == "1 s");
  assert(format_remaining(0) == "0 s");
  assert(format_remaining(-5) == "0 s");
  assert(format_remaining(60 * kMin) == "1 h");
  assert(format_remaining(90 * kMin) == "1 h 30 min");
}

void test_duration_labels_read_the_same_as_the_countdown() {
  assert(format_sleep_minutes(0) == "Off");
  assert(format_sleep_minutes(15) == "15 min");
  assert(format_sleep_minutes(60) == "1 h");
  assert(format_sleep_minutes(90) == "1 h 30 min");
  assert(format_sleep_minutes(120) == "2 h");
  assert(format_sleep_minutes(0.5) == "30 s");
}

void test_apply_action_arms_disarms_and_refuses_the_rest() {
  SleepTimer t = make();
  assert(t.apply_action("sleep.timer=15", 1000));
  assert(t.armed() && t.selected_minutes() == 15);
  assert(t.remaining_ms(1000) == 15 * kMin);

  assert(t.apply_action("sleep.timer=0", 1000));
  assert(!t.armed());
  // Already off: nothing changed, so the caller does not repaint or persist.
  assert(!t.apply_action("sleep.timer=0", 1000));

  // Another namespace's action must fall through untouched — osdmenu.cpp routes cc.* and sleep.*
  // through the same verdict, and a timer that swallowed a caption edit would silently drop it.
  assert(!t.apply_action("cc.type=author_first", 1000));
  assert(!t.apply_action("sleep.bogus=15", 1000));
  assert(!t.apply_action("sleep.timer", 1000));
  assert(!t.apply_action("sleep.timer=abc", 1000));
  assert(!t.armed());

  // A value the launcher never offered: a stale menu from before a config hot-swap, not a choice.
  assert(!t.apply_action("sleep.timer=7", 1000));
  assert(!t.armed());
}

void test_the_option_ladder_offers_off_first() {
  SleepTimer t = make();
  const std::vector<SleepOption> opts = t.options();
  assert(opts.size() == 4);
  assert(opts[0].value == "0" && opts[0].label == "Off");
  assert(opts[1].value == "5" && opts[1].label == "5 min");
  assert(opts[3].value == "30");
  // The stored value must parse back to the ladder entry, or apply_action would reject the menu's
  // own echo.
  assert(t.apply_action("sleep.timer=" + opts[2].value, 0));
  assert(t.selected_minutes() == 15);
}

void test_parse_options_sorts_dedupes_and_drops_junk() {
  const std::vector<double> v = parse_sleep_options(" 30, 5 ,15,5, ,abc,-1,0");
  assert(v.size() == 3);
  assert(v[0] == 5 && v[1] == 15 && v[2] == 30);
  // Trailing junk is rejected whole rather than half-read as 15.
  assert(parse_sleep_options("15min").empty());
  assert(parse_sleep_options("").empty());
  assert(parse_sleep_options("99999").empty());  // beyond any reachable countdown
  assert(parse_sleep_options("0.5").size() == 1);
}

void test_an_empty_ladder_falls_back_to_the_shipped_default() {
  // A bad hot-swap must degrade to the shipped behaviour, not to a menu with only "Off" in it.
  SleepTimer t(parse_sleep_options("garbage"), 60);
  assert(t.options().size() == default_sleep_options().size() + 1);
  assert(t.apply_action("sleep.timer=15", 0));
}

void test_osd_model_is_json_the_page_can_render() {
  SleepTimer t = make();
  const std::string off = t.osd_model_json(0);
  assert(off.find("\"ns\":\"sleep\"") != std::string::npos);
  assert(off.find("\"status\":\"Off\"") != std::string::npos);
  assert(off.find("\"kind\":\"combo\"") != std::string::npos);
  assert(off.find("\"key\":\"timer\"") != std::string::npos);
  assert(off.find("\"value\":\"0\"") != std::string::npos);

  t.arm(15, 0);
  const std::string armed = t.osd_model_json(0);
  assert(armed.find("\"status\":\"Stops in 15 min\"") != std::string::npos);
  assert(armed.find("\"value\":\"15\"") != std::string::npos);
}

}  // namespace
}  // namespace deckback

DECKBACK_TEST_MAIN(sleeptimer) {
  using namespace deckback;
  test_a_fresh_timer_is_off();
  test_it_fires_once_at_the_deadline_and_disarms();
  test_a_countdown_that_elapsed_during_suspend_is_over_not_restarted();
  test_the_warning_fires_once_inside_the_lead();
  test_no_warning_for_a_duration_no_longer_than_the_lead();
  test_warn_disabled_is_honoured();
  test_the_lead_is_readable_so_the_toast_can_state_it();
  test_rearming_resets_both_edges();
  test_remaining_and_status_count_down();
  test_remaining_rounds_up_so_a_fresh_timer_shows_what_was_picked();
  test_duration_labels_read_the_same_as_the_countdown();
  test_apply_action_arms_disarms_and_refuses_the_rest();
  test_the_option_ladder_offers_off_first();
  test_parse_options_sorts_dedupes_and_drops_junk();
  test_an_empty_ladder_falls_back_to_the_shipped_default();
  test_osd_model_is_json_the_page_can_render();
  std::printf("sleeptimer: all assertions passed\n");
  return 0;
}
