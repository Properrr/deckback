#include "player.hpp"

#include <chrono>
#include <ctime>
#include <format>

#include "log.hpp"
#include "netprobe.hpp"
#include "platform.hpp"

namespace deckback {
namespace {

// CLOCK_BOOTTIME (unlike CLOCK_MONOTONIC) keeps counting across system suspend, so the delta
// between on_suspend and on_resume is the real wall-clock time the Deck slept.
long boottime_ms() {
  timespec ts{};
  clock_gettime(CLOCK_BOOTTIME, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1'000'000L;
}

// One round trip, three signals, packed into a bitmask (see decode_play_state):
//   bit 0  playing            — a <video> is actively presenting frames (drives the idle inhibitor)
//   bit 1  player_open        — the watch view is up
//   bit 2  text_input_focused — a text field has focus
//
// `player_open` keys off Leanback's hash route. That route shape is *verified*, not guessed: the
// deep-link test observed `#/watch?v=<id>` with readyState=4 on-Deck (m114.md, 2026-07-08). It is
// still a Leanback implementation detail, so it is a layer-selection signal only — nothing about
// the keys we dispatch depends on it, and if the route changes we fall back to Browse, i.e. today's
// context-free behavior.
constexpr const char* kPlayStateExpr =
    "(function(){"
    "var v=document.querySelector('video');"
    "var playing=!!(v&&!v.paused&&!v.ended&&v.readyState>2);"
    "var open=!!v&&location.hash.indexOf('/watch')>=0;"
    "var a=document.activeElement;"
    "var t=!!(a&&(a.isContentEditable||/^(input|textarea)$/i.test(a.tagName||'')));"
    "return (playing?1:0)|(open?2:0)|(t?4:0);})()";

// Pause and return the current position so we can log a checkpoint; -1 when there is no video.
constexpr const char* kPauseExpr =
    "(function(){var v=document.querySelector('video');"
    "if(!v)return -1;v.pause();return v.currentTime;})()";

// Resume if paused; returns whether a video element was present.
constexpr const char* kPlayExpr =
    "(function(){var v=document.querySelector('video');"
    "if(v&&v.paused){v.play();}return !!v;})()";

}  // namespace

PlayState decode_play_state(double bitmask) {
  PlayState s;
  // NaN fails every comparison, and a negative value means "no result" — both decode to all-false.
  if (!(bitmask >= 0)) return s;
  const int b = static_cast<int>(bitmask);
  s.playing = (b & 1) != 0;
  s.player_open = (b & 2) != 0;
  s.text_input_focused = (b & 4) != 0;
  return s;
}

PlayState PlayerController::last_state() const {
  return decode_play_state(static_cast<double>(state_bits_.load(std::memory_order_relaxed)));
}

PlayerController::PlayerController(Platform& platform, std::string host, int port, int poll_ms,
                                   bool synthetic_fallback, ResumeProbe resume_probe,
                                   long reload_after_suspend_ms)
    : platform_(platform),
      client_(std::move(host), port),
      poll_ms_(poll_ms < 100 ? 100 : poll_ms),
      synthetic_fallback_(synthetic_fallback),
      resume_probe_(std::move(resume_probe)),
      reload_after_suspend_ms_(reload_after_suspend_ms) {}

PlayerController::~PlayerController() { stop(); }

void PlayerController::start() {
  {
    std::lock_guard lk(mu_);
    if (started_) return;
    started_ = true;
    stop_ = false;
  }
  thread_ = std::thread([this] { poll_loop(); });
}

void PlayerController::stop() {
  {
    std::lock_guard lk(mu_);
    if (!started_) return;
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  std::lock_guard lk(mu_);
  started_ = false;
}

bool PlayerController::poll_once() {
  // value_or(-1): an unreachable engine must decode to all-false, not to a stale or lucky value.
  const PlayState s = decode_play_state(client_.eval_number(kPlayStateExpr).value_or(-1));
  state_bits_.store((s.playing ? 1 : 0) | (s.player_open ? 2 : 0) | (s.text_input_focused ? 4 : 0),
                    std::memory_order_relaxed);

  platform_.set_idle_inhibited(s.playing);
  if (s.playing && synthetic_fallback_ && !platform_.backend_live() && !warned_synthetic_) {
    warn("player: idle-inhibit backend unavailable; synthetic-activity fallback not implemented");
    warned_synthetic_ = true;
  }
  if (layers_) {
    const Layer l = resolve_layer(s.player_open, s.text_input_focused);
    if (layers_->set(l)) info(std::format("player: input layer -> {}", layer_name(l)));
  }
  return s.playing;
}

void PlayerController::poll_loop() {
  std::unique_lock lk(mu_);
  while (!stop_) {
    lk.unlock();
    poll_once();
    lk.lock();
    cv_.wait_for(lk, std::chrono::milliseconds(poll_ms_), [this] { return stop_; });
  }
  // Leaving the loop: don't leave a stale inhibitor held.
  lk.unlock();
  platform_.set_idle_inhibited(false);
}

std::optional<double> PlayerController::on_suspend() {
  suspend_boottime_ms_ = boottime_ms();
  auto pos = client_.eval_number(kPauseExpr);
  if (pos && *pos >= 0) {
    info(std::format("player: paused + checkpointed at {:.1f}s before suspend", *pos));
    return pos;
  }
  info("player: suspend — no active video to checkpoint");
  return std::nullopt;
}

bool PlayerController::on_resume() {
  if (resume_probe_.timeout_ms > 0) {
    if (wait_online(resume_probe_.host, resume_probe_.port, resume_probe_.timeout_ms))
      info(std::format("player: network reachable ({}:{}) — nudging playback", resume_probe_.host,
                       resume_probe_.port));
    else
      warn(std::format("player: network still down after {} ms — nudging anyway",
                       resume_probe_.timeout_ms));
  }

  // After a long sleep, YouTube's stream URLs / auth token may be stale — a bare play() nudge would
  // just hit a dead <video>. Reload instead so Leanback re-fetches everything. Gated off by
  // default.
  if (reload_after_suspend_ms_ > 0 && suspend_boottime_ms_ > 0) {
    const long slept = boottime_ms() - suspend_boottime_ms_;
    if (slept >= reload_after_suspend_ms_) {
      const bool ok = client_.reload();
      info(ok ? std::format("player: slept {} ms (>= {} ms) — reloaded to refresh stream/token",
                            slept, reload_after_suspend_ms_)
              : "player: reload after long suspend failed (engine unreachable?)");
      return ok;
    }
  }

  const bool ok = client_.eval_void(kPlayExpr);
  info(ok ? "player: resume nudge sent" : "player: resume nudge failed (engine unreachable?)");
  return ok;
}

}  // namespace deckback
