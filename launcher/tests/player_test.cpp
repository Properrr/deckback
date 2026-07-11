#include "player.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "fake_cdp_server.hpp"
#include "netprobe.hpp"
#include "platform.hpp"

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

// ---- play-state bitmask + input-layer publication
// ------------------------------------------------

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

  // An unreachable engine (eval_number -> nullopt -> -1) and a NaN must both decode to all-false,
  // which is the Browse layer and no idle inhibitor. Never guess a state we could not observe.
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

// An unreachable engine must fall back to Browse, not strand the input in the last-seen layer:
// otherwise a crash during playback would leave every button on the player bindings forever.
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

// With no engine reachable we must never claim playing / hold an inhibitor, and hooks fail cleanly.
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
  // Wait until at least one tick has held the inhibitor (bounded, no fixed sleep race).
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

  // Dead port: wait_online must return false and respect its budget (not hang).
  auto t0 = std::chrono::steady_clock::now();
  assert(!wait_online("127.0.0.1", 8, 600));
  auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
          .count();
  assert(elapsed < 3000);  // bounded by the 600ms budget (+ backoff slack), nowhere near hanging

  assert(wait_online("127.0.0.1", 8, 0));  // timeout 0 disables the wait -> immediate true
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

// After a suspend longer than the reload threshold, resume reloads (refresh stream/token) instead
// of a bare play() nudge. A 1 ms threshold + a short sleep guarantees the boottime delta exceeds
// it.
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

}  // namespace

int main() {
  test_poll_tracks_playstate();
  test_decode_play_state();
  test_poll_publishes_input_layer();
  test_layer_falls_back_to_browse_when_engine_dies();
  test_suspend_resume_hooks();
  test_engine_unreachable();
  test_thread_lifecycle();
  test_netprobe();
  test_resume_with_probe();
  test_resume_reloads_after_long_suspend();
  test_resume_no_reload_when_disabled();
  std::puts("player_test: ok");
  return 0;
}
