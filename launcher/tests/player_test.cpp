#include "player.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "fake_cdp_server.hpp"
#include "harness.hpp"
#include "netprobe.hpp"
#include "platform.hpp"
#include "util.hpp"

using namespace deckback;

namespace {

// Records idle-inhibit reconciliations so the test can assert what the poll loop drove.
class FakePlatform final : public Platform {
 public:
  void on_suspend(std::function<void()>) override {}
  void on_resume(std::function<void()>) override {}
  void set_idle_inhibited(bool inhibited) override {
    inhibited_.store(inhibited);
    calls_.fetch_add(1);
  }
  bool backend_live() const override { return true; }
  int selftest() override { return 0; }

  bool inhibited() const { return inhibited_.load(); }
  int calls() const { return calls_.load(); }

 private:
  std::atomic<bool> inhibited_{false};
  std::atomic<int> calls_{0};
};

// A playing video holds the idle inhibitor; pausing releases it.
void test_poll_tracks_playstate() {
  testing::FakeServer server;
  FakePlatform plat;
  PlayerController pc(plat, "127.0.0.1", server.port(), 1000, /*synthetic_fallback=*/false);

  server.set_playing(true);
  assert(pc.poll_once() == true);
  assert(plat.inhibited() == true);

  server.set_playing(false);
  assert(pc.poll_once() == false);
  assert(plat.inhibited() == false);
}

// ---- play-state bitmask and input layer ---------------------------------------------------------

void test_decode_play_state() {
  PlayState s = decode_play_state(0);
  assert(!s.playing && !s.player_open && !s.text_input_focused);

  s = decode_play_state(1);
  assert(s.playing && !s.player_open && !s.text_input_focused);
  s = decode_play_state(2);
  assert(!s.playing && s.player_open && !s.text_input_focused);
  s = decode_play_state(4);
  assert(!s.playing && !s.player_open && s.text_input_focused);
  s = decode_play_state(7);
  assert(s.playing && s.player_open && s.text_input_focused);

  // An unreachable engine (-1) and NaN decode to all-false: Browse, no idle inhibitor.
  for (double bad : {-1.0, std::nan("")}) {
    s = decode_play_state(bad);
    assert(!s.playing && !s.player_open && !s.text_input_focused);
  }
}

// The poll publishes the observed context so the input layer can switch keymaps.
void test_poll_publishes_input_layer() {
  testing::FakeServer server;
  FakePlatform plat;
  LayerState layers;
  PlayerController pc(plat, "127.0.0.1", server.port(), 1000, false);
  pc.set_layer_sink(&layers);
  assert(layers.get() == Layer::Browse);  // before any poll

  server.set_playing(false);
  server.set_player_open(false);
  server.set_text_focused(false);
  pc.poll_once();
  assert(layers.get() == Layer::Browse);

  server.set_player_open(true);
  server.set_playing(true);
  pc.poll_once();
  assert(layers.get() == Layer::Player);
  assert(pc.last_state().player_open && pc.last_state().playing);

  // A focused text field wins even while the video plays.
  server.set_text_focused(true);
  pc.poll_once();
  assert(layers.get() == Layer::Osk);

  server.set_text_focused(false);
  server.set_player_open(false);
  server.set_playing(false);
  pc.poll_once();
  assert(layers.get() == Layer::Browse);
}

// An unreachable engine falls back to Browse rather than keeping the last-seen layer.
void test_layer_falls_back_to_browse_when_engine_dies() {
  LayerState layers;
  FakePlatform plat;
  {
    testing::FakeServer server;
    PlayerController pc(plat, "127.0.0.1", server.port(), 1000, false);
    pc.set_layer_sink(&layers);
    server.set_player_open(true);
    pc.poll_once();
    assert(layers.get() == Layer::Player);
  }
  // Server destroyed: the port is dead.
  PlayerController dead(plat, "127.0.0.1", 8, 1000, false);
  dead.set_layer_sink(&layers);
  assert(dead.poll_once() == false);
  assert(layers.get() == Layer::Browse);
}

// Suspend pauses and checkpoints the position; resume nudges playback.
void test_suspend_resume_hooks() {
  testing::FakeServer server;
  FakePlatform plat;
  PlayerController pc(plat, "127.0.0.1", server.port(), 1000, false);

  auto checkpoint = pc.on_suspend();
  assert(checkpoint.has_value() && *checkpoint == 123.5);
  assert(pc.on_resume() == true);
}

// With no engine reachable: never playing, no inhibitor, and hooks fail cleanly.
void test_engine_unreachable() {
  FakePlatform plat;
  PlayerController pc(plat, "127.0.0.1", 8, 1000, false);
  assert(pc.poll_once() == false);
  assert(plat.inhibited() == false);
  assert(!pc.on_suspend().has_value());
  assert(pc.on_resume() == false);
}

// The poll thread starts, ticks, and stops cleanly, leaving the inhibitor released.
void test_thread_lifecycle() {
  testing::FakeServer server;
  server.set_playing(true);
  FakePlatform plat;
  PlayerController pc(plat, "127.0.0.1", server.port(), 100, false);

  pc.start();
  // Wait (bounded) until a tick has held the inhibitor.
  for (int i = 0; i < 100 && !plat.inhibited(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  assert(plat.inhibited() == true);

  pc.stop();  // must join and release
  assert(plat.inhibited() == false);
  pc.stop();  // idempotent
}

// The network probe: a listening port is reachable; a dead port times out within its budget.
void test_netprobe() {
  testing::FakeServer server;  // a live loopback listener
  assert(tcp_reachable("127.0.0.1", server.port(), 500));
  assert(wait_online("127.0.0.1", server.port(), 2000));

  // Dead port: wait_online returns false within its budget.
  auto t0 = std::chrono::steady_clock::now();
  assert(!wait_online("127.0.0.1", 8, 600));
  auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
          .count();
  assert(elapsed < 3000);  // 600 ms budget plus backoff slack

  assert(wait_online("127.0.0.1", 8, 0));  // timeout 0 disables the wait -> immediate true
}

// ---- resume off the logind thread ---------------------------------------------------------------

int nudges(testing::FakeServer& server) {
  int n = 0;
  for (const std::string& r : server.take_requests())
    if (r.find("v.play()") != std::string::npos) ++n;
  return n;
}

bool wait_for(const std::function<bool()>& cond, long ms) {
  const long deadline = mono_ms() + ms;
  while (mono_ms() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return cond();
}

// request_resume returns at once; the poll thread does the network wait and the nudge.
void test_resume_request_returns_at_once_and_nudges_on_the_poll_thread() {
  testing::FakeServer server;
  FakePlatform plat;
  PlayerController pc(plat, "127.0.0.1", server.port(), 5000, false,
                      ResumeProbe{"127.0.0.1", 8, 600});  // never reachable: the full 600 ms wait
  pc.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // into its 5 s poll wait
  server.take_requests();
  const long t0 = mono_ms();
  pc.request_resume();
  assert(mono_ms() - t0 < 50);
  int seen = 0;
  // Nudges once the probe budget is spent, not a 5 s poll interval later.
  assert(wait_for([&] { return (seen += nudges(server)) > 0; }, 2000));
  pc.stop();
}

// stop() interrupts the resume probe's waits.
void test_stop_does_not_wait_out_the_resume_probe() {
  testing::FakeServer server;
  FakePlatform plat;
  PlayerController pc(plat, "127.0.0.1", server.port(), 5000, false,
                      ResumeProbe{"127.0.0.1", 8, 8000});  // nothing listens on port 8
  pc.start();
  pc.request_resume();
  std::this_thread::sleep_for(std::chrono::milliseconds(150));  // inside the probe's wait
  const long t0 = mono_ms();
  pc.stop();
  assert(mono_ms() - t0 < 1000);
  assert(nudges(server) == 0);
}

// A suspend during the resume's network wait cancels its nudge.
void test_a_suspend_during_the_resume_wait_cancels_the_nudge() {
  testing::FakeServer server;
  FakePlatform plat;
  PlayerController pc(plat, "127.0.0.1", server.port(), 5000, false,
                      ResumeProbe{"127.0.0.1", 8, 1200});
  pc.start();
  pc.request_resume();
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  pc.on_suspend();
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // past the probe budget
  assert(nudges(server) == 0);
  pc.stop();
}

// Resume with a network probe pointed at a live port still nudges playback.
void test_resume_with_probe() {
  testing::FakeServer server;
  FakePlatform plat;
  PlayerController pc(plat, "127.0.0.1", server.port(), 1000, false,
                      ResumeProbe{"127.0.0.1", server.port(), 2000});
  assert(pc.on_resume() == true);
  assert(!server.saw_reload());  // default (reload_after=0) -> nudge, never reload
}

// After a suspend longer than the reload threshold, resume reloads instead of nudging. A 1 ms
// threshold plus a short sleep guarantees the boottime delta exceeds it.
void test_resume_reloads_after_long_suspend() {
  testing::FakeServer server;
  FakePlatform plat;
  PlayerController pc(plat, "127.0.0.1", server.port(), 1000, false, ResumeProbe{},
                      /*reload_after_suspend_ms=*/1);
  pc.on_suspend();  // records the CLOCK_BOOTTIME baseline
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(pc.on_resume() == true);
  assert(server.saw_reload());  // took the reload path
}

// With the feature disabled (0), even a long sleep only nudges — never reloads.
void test_resume_no_reload_when_disabled() {
  testing::FakeServer server;
  FakePlatform plat;
  PlayerController pc(plat, "127.0.0.1", server.port(), 1000, false, ResumeProbe{},
                      /*reload_after_suspend_ms=*/0);
  pc.on_suspend();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(pc.on_resume() == true);
  assert(!server.saw_reload());
}

// The ASCII core of kKeepAwakeToast, so JSON-escaping of the em-dash cannot break the match.
constexpr std::string_view kKeepAwakeNeedle = "Keep-awake helper not running";

bool any_request_contains(const std::vector<std::string>& reqs, std::string_view needle) {
  for (const auto& r : reqs)
    if (r.find(needle) != std::string_view::npos) return true;
  return false;
}

// Helper missing + a video playing: warn exactly once, however long playback runs.
void test_keep_awake_warns_once_when_helper_inactive() {
  testing::FakeServer server;
  FakePlatform plat;
  PlayerController pc(plat, "127.0.0.1", server.port(), 1000, false);
  int probes = 0;
  pc.set_keep_awake_probe([&probes] {
    ++probes;
    return UnitState::Inactive;
  });
  server.set_playing(true);
  server.take_requests();
  assert(pc.poll_once() == true);
  assert(pc.poll_once() == true);
  assert(probes == 1);
  assert(any_request_contains(server.take_requests(), kKeepAwakeNeedle));
}

// Helper running: probed once, never announced.
void test_keep_awake_silent_when_helper_active() {
  testing::FakeServer server;
  FakePlatform plat;
  PlayerController pc(plat, "127.0.0.1", server.port(), 1000, false);
  int probes = 0;
  pc.set_keep_awake_probe([&probes] {
    ++probes;
    return UnitState::Active;
  });
  server.set_playing(true);
  server.take_requests();
  assert(pc.poll_once() == true);
  assert(probes == 1);
  assert(!any_request_contains(server.take_requests(), kKeepAwakeNeedle));
}

// Unknown (could not determine) stays silent.
void test_keep_awake_silent_when_state_unknown() {
  testing::FakeServer server;
  FakePlatform plat;
  PlayerController pc(plat, "127.0.0.1", server.port(), 1000, false);
  pc.set_keep_awake_probe([] { return UnitState::Unknown; });
  server.set_playing(true);
  server.take_requests();
  assert(pc.poll_once() == true);
  assert(!any_request_contains(server.take_requests(), kKeepAwakeNeedle));
}

// Nothing playing: nothing to keep awake, so the helper is never probed.
void test_keep_awake_not_probed_while_idle() {
  testing::FakeServer server;
  FakePlatform plat;
  PlayerController pc(plat, "127.0.0.1", server.port(), 1000, false);
  int probes = 0;
  pc.set_keep_awake_probe([&probes] {
    ++probes;
    return UnitState::Inactive;
  });
  server.set_playing(false);
  server.take_requests();
  assert(pc.poll_once() == false);
  assert(probes == 0);
  assert(!any_request_contains(server.take_requests(), kKeepAwakeNeedle));
}

// The verdict from the helper's heartbeat file age. A missing file is Unknown, not Inactive.
void test_decide_keep_awake() {
  assert(decide_keep_awake(std::nullopt) == UnitState::Unknown);
  assert(decide_keep_awake(0) == UnitState::Active);
  assert(decide_keep_awake(kKeepAwakeStaleMs - 1) == UnitState::Active);
  assert(decide_keep_awake(kKeepAwakeStaleMs) == UnitState::Active);  // boundary is inclusive
  assert(decide_keep_awake(kKeepAwakeStaleMs + 1) == UnitState::Inactive);
  // The window spans several helper ticks, so a just-resumed helper is not reported dead.
  assert(kKeepAwakeStaleMs >= 5 * 25'000);
}

// The heartbeat path must match scripts/idle-nudge.py::heartbeat_path.
void test_keep_awake_heartbeat_path() {
  const char* const prev = std::getenv("XDG_DATA_HOME");
  ::setenv("XDG_DATA_HOME", "/home/deck/.var/app/io.github.properrr.deckback/data", 1);
  assert(keep_awake_heartbeat_path() ==
         "/home/deck/.var/app/io.github.properrr.deckback/data/deckback/idle-nudge.alive");
  ::unsetenv("XDG_DATA_HOME");
  ::setenv("HOME", "/home/deck", 1);
  assert(keep_awake_heartbeat_path() == "/home/deck/.local/share/deckback/idle-nudge.alive");
  if (prev) ::setenv("XDG_DATA_HOME", prev, 1);
}

}  // namespace

DECKBACK_TEST_MAIN(player) {
  test_poll_tracks_playstate();
  test_decode_play_state();
  test_poll_publishes_input_layer();
  test_layer_falls_back_to_browse_when_engine_dies();
  test_suspend_resume_hooks();
  test_engine_unreachable();
  test_thread_lifecycle();
  test_netprobe();
  test_resume_with_probe();
  test_resume_request_returns_at_once_and_nudges_on_the_poll_thread();
  test_stop_does_not_wait_out_the_resume_probe();
  test_a_suspend_during_the_resume_wait_cancels_the_nudge();
  test_resume_reloads_after_long_suspend();
  test_resume_no_reload_when_disabled();
  test_keep_awake_warns_once_when_helper_inactive();
  test_keep_awake_silent_when_helper_active();
  test_keep_awake_silent_when_state_unknown();
  test_keep_awake_not_probed_while_idle();
  test_decide_keep_awake();
  test_keep_awake_heartbeat_path();
  std::puts("player_test: ok");
  return 0;
}
