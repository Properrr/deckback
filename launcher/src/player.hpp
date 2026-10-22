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

// Host unit installed by scripts/install-idle-nudge.sh; without it SteamOS dims and suspends
// mid-video and nothing in the sandbox can prevent that.
inline constexpr const char* kKeepAwakeUnit = "deckback-idle-nudge.service";

// Heartbeat file the helper touches in the app's data dir. Used instead of asking systemd, which
// would need --talk-name=org.freedesktop.systemd1: flatpak-portal refuses any self-update that
// widens the sandbox.
inline constexpr const char* kKeepAwakeHeartbeat = "idle-nudge.alive";
// The helper refreshes the heartbeat every 25 s (scripts/idle-nudge.py). Six missed polls count as
// dead, because after a resume the file stays stale until the helper's next tick.
inline constexpr long kKeepAwakeStaleMs = 150'000;

// Heartbeat path under $XDG_DATA_HOME/deckback; in the Flatpak that is ~/.var/app/<id>/data, the
// same file on the host.
std::string keep_awake_heartbeat_path();

// Verdict from one heartbeat observation; `age_ms` is nullopt when the file does not exist. Absent
// reads as Unknown, never Inactive: a helper that writes no heartbeat cannot be told apart from a
// missing one.
UnitState decide_keep_awake(std::optional<long> age_ms, long stale_ms = kKeepAwakeStaleMs);

// Default keep-awake probe: decide_keep_awake() over the heartbeat file.
UnitState keep_awake_state();

// On resume, wait until host:port is reachable before nudging the player (timeout_ms 0 = skip).
struct ResumeProbe {
  std::string host;
  int port = 443;
  int timeout_ms = 0;
};

// What one poll tick observed about the page, fetched as one bitmask in a single CDP round trip so
// the fields are consistent with each other.
struct PlayState {
  bool playing = false;      // a <video> is actively presenting frames -> hold the idle inhibitor
  bool player_open = false;  // the watch view is up (route + a <video> element)
  bool text_input_focused = false;  // a text field has focus -> the OSK layer
};

// Decode the bitmask kPlayStateExpr returns. A negative/NaN value (engine unreachable, no result)
// decodes to all-false, which resolves to the Browse layer and no inhibitor — the safe default.
PlayState decode_play_state(double bitmask);

// The power contract over CDP: a poll thread holds the logind idle inhibitor while a video plays;
// on_suspend() pauses and checkpoints while the delay inhibitor is held; on_resume() nudges
// playback. DevToolsClient is internally synchronized, so the poll thread and the logind callbacks
// may call in concurrently.
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

  // logind hooks. on_suspend returns the checkpointed position (nullopt if unavailable) and must
  // finish while logind's delay lock is held. on_resume returns whether the play nudge was accepted
  // and may wait up to resume_probe.timeout_ms for the network, so the logind thread calls
  // request_resume() instead: it hands the resume to the poll thread and returns at once. A suspend
  // during the wait cancels the nudge.
  std::optional<double> on_suspend();
  bool on_resume();
  void request_resume();

 private:
  void poll_loop();
  bool resume(unsigned suspend_gen);  // on_resume() against the suspend count it was asked under

  Platform& platform_;
  DevToolsClient client_;
  int poll_ms_;
  bool synthetic_fallback_;
  ResumeProbe resume_probe_;
  // After a suspend at least this long (CLOCK_BOOTTIME; 0 = disabled), reload on resume instead of
  // nudging, so stale stream URLs / auth tokens are refreshed.
  long reload_after_suspend_ms_ = 0;
  // CLOCK_BOOTTIME at the last on_suspend (0 = none seen). Written on the logind thread, read on
  // the poll thread.
  std::atomic<long> suspend_boottime_ms_{0};
  std::atomic<unsigned> suspend_gen_{0};  // bumped by every on_suspend
  std::atomic<bool> resume_pending_{false};
  std::atomic<unsigned> resume_gen_{0};  // suspend_gen_ when the pending resume was requested
  bool warned_synthetic_ = false;
  std::function<UnitState()> keep_awake_probe_;
  bool warned_keep_awake_ = false;
  LayerState* layers_ = nullptr;  // optional context sink for the input layer
  // Raw bitmask: std::atomic<PlayState> is not lock-free and would need -latomic on GCC.
  std::atomic<int> state_bits_{0};

  WorkerThread worker_;
};

}  // namespace deckback
