#pragma once
#include <atomic>
#include <functional>
#include <optional>
#include <string>

#include "devtools.hpp"
#include "layers.hpp"
#include "platform.hpp"
#include "worker.hpp"

namespace deckback {

// The host unit scripts/install-idle-nudge.sh installs; without it SteamOS dims and suspends
// mid-video and nothing in the sandbox can stop it (findings durable/keep-awake.md).
inline constexpr const char* kKeepAwakeUnit = "deckback-idle-nudge.service";

// Default keep-awake probe. Unknown when we may not ask, which must not produce a warning.
UnitState keep_awake_state();

// On resume, wait until host:port is reachable before nudging the player (timeout_ms 0 = skip).
struct ResumeProbe {
  std::string host;
  int port = 443;
  int timeout_ms = 0;
};

// What one poll tick observed about the page. Fetched as a single bitmask in one CDP round trip:
// three separate Runtime.evaluate calls per tick would triple the poll cost and could observe an
// inconsistent page between them.
struct PlayState {
  bool playing = false;      // a <video> is actively presenting frames -> hold the idle inhibitor
  bool player_open = false;  // the watch view is up (route + a <video> element)
  bool text_input_focused = false;  // a text field has focus -> the OSK layer
};

// Decode the bitmask kPlayStateExpr returns. A negative/NaN value (engine unreachable, no result)
// decodes to all-false, which resolves to the Browse layer and no inhibitor — the safe default.
PlayState decode_play_state(double bitmask);

// Drives the Phase 6 power contract through the DevTools bridge (doc §2 P6):
//   * a background thread polls the player's play-state and holds/releases the logind idle
//     inhibitor via Platform so the screen never dims mid-video;
//   * on_suspend() pauses the video and reads its position (checkpoint) while the delay inhibitor
//     is still held;
//   * on_resume() nudges playback back.
// All engine access goes through one DevToolsClient, which is internally synchronized, so the poll
// thread and the logind callbacks may call in concurrently.
class PlayerController {
 public:
  PlayerController(Platform& platform, std::string host, int port, int poll_ms,
                   bool synthetic_fallback, ResumeProbe resume_probe = ResumeProbe{},
                   long reload_after_suspend_ms = 0);
  ~PlayerController();

  PlayerController(const PlayerController&) = delete;
  PlayerController& operator=(const PlayerController&) = delete;

  // Publish the observed context to `layers` on every poll tick, so the input layer can switch
  // keymaps. Must be called before start(); the LayerState must outlive this controller.
  void set_layer_sink(LayerState* layers) { layers_ = layers; }

  // Checked once, on the first tick that observes playback, and warned about only if Inactive.
  // Leave unset to disable the check entirely.
  void set_keep_awake_probe(std::function<UnitState()> probe) {
    keep_awake_probe_ = std::move(probe);
  }

  void start();  // launch the poll thread
  void stop();   // signal + join (idempotent; also called by the destructor)

  // One poll tick: query play-state, reconcile the idle inhibitor, publish the layer. Returns the
  // play-state it applied (false when the engine is unreachable — we never inhibit on an unknown
  // state).
  bool poll_once();

  // The last state poll_once() observed. Exposed for tests and logging.
  PlayState last_state() const;

  // logind hooks. on_suspend returns the checkpointed position (nullopt if unavailable); on_resume
  // returns whether the play nudge was accepted. Return values are for tests/logging.
  std::optional<double> on_suspend();
  bool on_resume();

 private:
  void poll_loop();

  Platform& platform_;
  DevToolsClient client_;
  int poll_ms_;
  bool synthetic_fallback_;
  ResumeProbe resume_probe_;
  // After a suspend at least this long (0 = disabled), reload on resume so stale stream URLs / auth
  // tokens are refreshed instead of nudging a dead <video>. Duration is measured with
  // CLOCK_BOOTTIME.
  long reload_after_suspend_ms_ = 0;
  long suspend_boottime_ms_ = 0;  // CLOCK_BOOTTIME at the last on_suspend (0 = none seen)
  bool warned_synthetic_ = false;
  std::function<UnitState()> keep_awake_probe_;
  bool warned_keep_awake_ = false;
  LayerState* layers_ = nullptr;  // optional context sink for the input layer
  // Kept as the raw bitmask rather than std::atomic<PlayState>: a 3-byte struct is not lock-free
  // and would drag in -latomic on GCC for no benefit.
  std::atomic<int> state_bits_{0};

  WorkerThread worker_;
};

}  // namespace deckback
