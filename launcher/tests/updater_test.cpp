// L0 smoke for the self-updater lifecycle. The portal path itself (sd-bus → the Flatpak update
// portal) is integration-level and is verified on a Deck — `--selftest-update` plus a real update
// round-trip against a repo (findings/durable/self-update.md). Here we pin only the contract that
// holds off-Deck: the factory always yields a usable object, and the lifecycle is crash-free and
// idempotent regardless of whether a session bus / portal exists. Run under ASan (the test targets
// force -UNDEBUG) it also guards the thread-join lifetime — the exact bug an `exchange`-guarded
// stop() would have introduced (a self-exited thread destroyed while still joinable).
#include "updater.hpp"

#include <cassert>
#include <cstdio>
#include <string>

#include "harness.hpp"

using namespace deckback;

DECKBACK_TEST_MAIN(updater) {
  // The Progress.status decode the whole feature hinges on (findings/durable/self-update.md:
  // "correctness hinges on 2 == Done"). Pure + always-compiled, so this runs in CI where the
  // sd-bus portal path compiles out. An off-by-one here would silently never apply an update.
  assert(decode_progress_status(0) == UpdateProgress::Running);
  assert(decode_progress_status(1) == UpdateProgress::Empty);
  assert(decode_progress_status(2) == UpdateProgress::Done);
  assert(decode_progress_status(3) == UpdateProgress::Failed);
  assert(decode_progress_status(42) == UpdateProgress::Unknown);

  // Permission decode mirrors flatpak-portal's get_update_permission() exactly: only the FIRST
  // element counts, "yes"/"ask" match verbatim, any other value (including "no" and garbage) is
  // No, and a missing entry is Unset. A drift here and the seeder either forges consent over an
  // explicit "no" or never seeds at all.
  assert(decode_update_permission({}) == UpdatePermission::Unset);
  assert(decode_update_permission({"yes"}) == UpdatePermission::Yes);
  assert(decode_update_permission({"ask"}) == UpdatePermission::Ask);
  assert(decode_update_permission({"no"}) == UpdatePermission::No);
  assert(decode_update_permission({"YES"}) == UpdatePermission::No);
  assert(decode_update_permission({"granted"}) == UpdatePermission::No);
  assert(decode_update_permission({"no", "yes"}) == UpdatePermission::No);
  assert(decode_update_permission({"ask", "no"}) == UpdatePermission::Ask);

  assert(std::string(update_permission_name(UpdatePermission::Unset)) == "unset");
  assert(std::string(update_permission_name(UpdatePermission::Ask)) == "ask");
  assert(std::string(update_permission_name(UpdatePermission::Yes)) == "yes");
  assert(std::string(update_permission_name(UpdatePermission::No)) == "no");

  // /.flatpak-info parsing: the app id comes from `name=` inside `[Application]` only.
  assert(parse_flatpak_app_id("[Application]\n"
                              "name=io.github.properrr.deckback\n"
                              "runtime=runtime/org.freedesktop.Platform/x86_64/25.08\n") ==
         "io.github.properrr.deckback");
  assert(parse_flatpak_app_id("[Context]\nsockets=x11;\n"
                              "[Application]\r\n"
                              "name=io.github.properrr.deckback\r\n") ==
         "io.github.properrr.deckback");
  assert(parse_flatpak_app_id("[Instance]\n"
                              "name=not-the-app-id\n"
                              "[Application]\n"
                              "name=io.github.properrr.deckback") == "io.github.properrr.deckback");
  assert(parse_flatpak_app_id("[Application]\nruntime=whatever\n").empty());
  assert(parse_flatpak_app_id("name=orphan-before-any-section\n").empty());
  assert(parse_flatpak_app_id("").empty());

  // Reconnect backoff (durable/dbus-reconnect.md): exponential from 1s, doubling, capped at 60s.
  // The shift is clamped so a large attempt index is defined, not UB (review N9).
  assert(reconnect_delay_ms(0) == 1000);
  assert(reconnect_delay_ms(1) == 2000);
  assert(reconnect_delay_ms(2) == 4000);
  assert(reconnect_delay_ms(3) == 8000);
  assert(reconnect_delay_ms(4) == 16000);
  assert(reconnect_delay_ms(5) == 32000);
  assert(reconnect_delay_ms(6) == 60000);  // 64000 clamped to the 60s cap
  assert(reconnect_delay_ms(7) == 60000);
  assert(reconnect_delay_ms(100) == 60000);  // no shift overflow at large attempt indices

  // Give up (go inert until relaunch) only after 12 consecutive failures (~6-7 min of backoff).
  assert(!reconnect_should_give_up(0));
  assert(!reconnect_should_give_up(11));
  assert(reconnect_should_give_up(12));
  assert(reconnect_should_give_up(13));

  // create() always yields a usable object: the real backend where sd-bus is compiled in, else a
  // stub.
  auto up = Updater::create(UpdaterConfig{});
  assert(up);
  (void)up->backend_live();  // must not crash whether or not a session bus exists

  // stop() before start() is a no-op.
  up->stop();

  // start()+stop() must be safe even when the portal is unreachable: start() may launch a thread
  // that self-exits on a failed CreateUpdateMonitor, and stop() must still join it — and be
  // idempotent.
  up->start();
  up->stop();
  up->stop();

  // A notify-mode instance (auto_deploy=false) with a CDP port set is still a no-op off-Deck: no
  // toast can fire without a delivered update, and request_update() is safe before/after
  // start/stop.
  auto up2 = Updater::create(UpdaterConfig{.auto_deploy = false, .cdp_port = 65000});
  up2->request_update();  // no pending update, no portal: must not crash
  up2->start();
  up2->request_update();
  up2->stop();

  // An auto-deploy instance is lifecycle-safe off-Deck too: the consent seeder finds no
  // /.flatpak-info outside a sandbox and skips, and a missing portal still leaves the updater
  // inert.
  auto up3 = Updater::create(UpdaterConfig{.auto_deploy = true});
  up3->start();
  up3->stop();

  // UpdateState is the updater->UI hand-off: availability + the commit, nothing else. Dismissal
  // ("ignore this version") is keyed to the commit via the on-disk dot marker
  // (decide_notification), so a newer commit re-arms the pill without any state here.
  UpdateState state;
  assert(!state.available());
  assert(state.commit().empty());
  state.set_available(true, "aaaa1111");
  assert(state.available());
  assert(state.commit() == "aaaa1111");
  state.set_available(true, "bbbb2222");
  assert(state.commit() == "bbbb2222");

  std::puts("updater: lifecycle ok");
  return 0;
}
