// Self-updater pure decoders and lifecycle. The factory always yields a usable object, and
// start/stop is crash-free and idempotent whether or not a session bus or portal exists. The portal
// path itself needs a Deck.
#include "updater.hpp"

#include <cassert>
#include <cstdio>
#include <string>

#include "harness.hpp"

using namespace deckback;

DECKBACK_TEST_MAIN(updater) {
  // The portal's Progress.status codes (2 == Done).
  assert(decode_progress_status(0) == UpdateProgress::Running);
  assert(decode_progress_status(1) == UpdateProgress::Empty);
  assert(decode_progress_status(2) == UpdateProgress::Done);
  assert(decode_progress_status(3) == UpdateProgress::Failed);
  assert(decode_progress_status(42) == UpdateProgress::Unknown);

  // Mirrors flatpak-portal's get_update_permission(): only the first element counts, "yes"/"ask"
  // match verbatim, anything else is No, and a missing entry is Unset.
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

  // The generic keyfile read: [Instance] app-commit is the running commit.
  const std::string info =
      "[Application]\nname=io.github.properrr.deckback\n"
      "[Instance]\ninstance-id=123\napp-commit-extra=nope\napp-commit=abc123\n";
  assert(parse_flatpak_info_value(info, "Instance", "app-commit") == "abc123");  // not the prefix
  assert(parse_flatpak_info_value(info, "Application", "app-commit").empty());
  assert(
      parse_flatpak_info_value("[Instancex]\napp-commit=no\n", "Instance", "app-commit").empty());
  assert(parse_flatpak_info_value(info, "Instance", "missing").empty());
  assert(short_commit("0123456789abcdef") == "0123456789ab");
  assert(short_commit("abc") == "abc");

  // Reconnect backoff: doubling from 1 s, capped at 60 s, defined for any attempt index.
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

  // create() yields the real backend where sd-bus is compiled in, else a stub.
  auto up = Updater::create(UpdaterConfig{});
  assert(up);
  (void)up->backend_live();

  // stop() before start() is a no-op.
  up->stop();

  // start() may launch a thread that self-exits when the portal is unreachable; stop() still joins
  // it, idempotently.
  up->start();
  up->stop();
  up->stop();

  // Notify mode: request_update() is safe with no pending update, before and after start/stop.
  auto up2 = Updater::create(
      UpdaterConfig{.auto_deploy = false, .cdp_port = 65000, .on_terminal = nullptr});
  up2->request_update();
  up2->start();
  up2->request_update();
  up2->stop();

  // Auto-deploy outside a sandbox (no /.flatpak-info) stays inert.
  auto up3 = Updater::create(UpdaterConfig{.auto_deploy = true, .on_terminal = nullptr});
  up3->start();
  up3->stop();

  // A portal refusal because the update widens the sandbox. Keyed on the D-Bus error name, since
  // the message is localized.
  assert(is_permission_change_failure("org.freedesktop.DBus.Error.NotSupported", ""));
  assert(is_permission_change_failure(
      "", "Self update not supported, new version requires new permissions"));
  assert(is_permission_change_failure("org.freedesktop.DBus.Error.NotSupported",
                                      "Selbst-Update nicht unterstützt"));
  assert(!is_permission_change_failure("org.freedesktop.DBus.Error.Failed", "network unreachable"));
  assert(!is_permission_change_failure("", ""));

  // UpdateState carries availability and the commit from updater to UI.
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
