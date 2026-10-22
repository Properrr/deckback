#include "player.hpp"

#include <cstdlib>
#include <ctime>
#include <format>

#include "fileio.hpp"
#include "log.hpp"
#include "netprobe.hpp"
#include "overlay.hpp"
#include "platform.hpp"
#include "scripts.hpp"

namespace deckback {
namespace {

// Shown when the keep-awake helper is missing. One short line: the how-to-fix detail is in the log
// line and SUPPORT.md.
constexpr const char* kKeepAwakeToast =
    "Keep-awake helper not running \xE2\x80\x94 screen may dim during video";
constexpr int kKeepAwakeToastMs = 9000;

// CLOCK_BOOTTIME (unlike CLOCK_MONOTONIC) keeps counting across system suspend, so the delta
// between on_suspend and on_resume is the real wall-clock time the Deck slept.
long boottime_ms() {
  timespec ts{};
  clock_gettime(CLOCK_BOOTTIME, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1'000'000L;
}

// player_state.js returns a bitmask (decode_play_state). `player_open` keys off Leanback's
// `#/watch` route, so it only selects the input layer and falls back to Browse if that changes.
std::string play_state_js() { return ScriptLibrary::instance().render("player_state"); }
std::string pause_js() { return ScriptLibrary::instance().render("player_pause"); }
std::string play_js() { return ScriptLibrary::instance().render("player_play"); }

}  // namespace

std::string keep_awake_heartbeat_path() {
  std::string dir;
  if (const char* v = std::getenv("XDG_DATA_HOME"); v && *v)
    dir = std::string(v) + "/deckback";
  else if (const char* h = std::getenv("HOME"); h && *h)
    dir = std::string(h) + "/.local/share/deckback";
  else
    return {};
  return dir + "/" + kKeepAwakeHeartbeat;
}

UnitState decide_keep_awake(std::optional<long> age_ms, long stale_ms) {
  if (!age_ms) return UnitState::Unknown;
  return *age_ms <= stale_ms ? UnitState::Active : UnitState::Inactive;
}

UnitState keep_awake_state() {
  const std::string path = keep_awake_heartbeat_path();
  if (path.empty()) return UnitState::Unknown;
  return decide_keep_awake(file_age_ms(path));
}

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
  worker_.start([this] { poll_loop(); });
}

void PlayerController::stop() { worker_.stop(); }

bool PlayerController::poll_once() {
  // value_or(-1): an unreachable engine must decode to all-false, not to a stale or lucky value.
  const double raw = client_.eval_number(play_state_js()).value_or(-1);
  const int bits = raw >= 0 ? (static_cast<int>(raw) & 7) : 0;  // NaN fails the comparison too
  state_bits_.store(bits, std::memory_order_relaxed);
  const PlayState s = decode_play_state(bits);

  platform_.set_idle_inhibited(s.playing);
  if (s.playing && synthetic_fallback_ && !platform_.backend_live() && !warned_synthetic_) {
    warn("player: idle-inhibit backend unavailable; synthetic-activity fallback not implemented");
    warned_synthetic_ = true;
  }
  if (s.playing && keep_awake_probe_ && !warned_keep_awake_) {
    warned_keep_awake_ = true;
    if (keep_awake_probe_() == UnitState::Inactive) {
      warn(
          std::format("player: {} is not running — the screen may dim and the Deck may suspend "
                      "mid-video; install the keep-awake helper in Desktop Mode (docs/SUPPORT.md)",
                      kKeepAwakeUnit));
      show_toast(client_, kKeepAwakeToast, kKeepAwakeToastMs);
    }
  }
  if (layers_) {
    layers_->set_video_up(s.player_open);
    const Layer l = resolve_layer(s.player_open, s.text_input_focused);
    if (layers_->set(l)) info(std::format("player: input layer -> {}", layer_name(l)));
  }
  return s.playing;
}

void PlayerController::poll_loop() {
  do {
    if (resume_pending_.exchange(false)) resume(resume_gen_.load());
    poll_once();
  } while (!worker_.wait_or_stop(poll_ms_));
  // Leaving the loop: don't leave a stale inhibitor held.
  platform_.set_idle_inhibited(false);
}

std::optional<double> PlayerController::on_suspend() {
  suspend_gen_.fetch_add(1);
  suspend_boottime_ms_.store(boottime_ms());
  auto pos = client_.eval_number(pause_js());
  if (pos && *pos >= 0) {
    info(std::format("player: paused + checkpointed at {:.1f}s before suspend", *pos));
    return pos;
  }
  info("player: suspend — no active video to checkpoint");
  return std::nullopt;
}

void PlayerController::request_resume() {
  resume_gen_.store(suspend_gen_.load());
  resume_pending_.store(true);
  worker_.wake();
}

bool PlayerController::on_resume() { return resume(suspend_gen_.load()); }

bool PlayerController::resume(unsigned gen) {
  // Gives up early on stop() or on a newer suspend.
  const auto superseded = [&] { return suspend_gen_.load() != gen; };
  if (resume_probe_.timeout_ms > 0) {
    if (wait_online(resume_probe_.host, resume_probe_.port, resume_probe_.timeout_ms,
                    [&](int ms) { return worker_.wait_or_stop(ms) || superseded(); }))
      info(std::format("player: network reachable ({}:{}) — nudging playback", resume_probe_.host,
                       resume_probe_.port));
    else if (!superseded() && !worker_.stopping())
      warn(std::format("player: network still down after {} ms — nudging anyway",
                       resume_probe_.timeout_ms));
  }
  if (superseded() || worker_.stopping()) {
    info("player: resume overtaken by a suspend or shutdown — no nudge");
    return false;
  }

  // After a long sleep the stream URLs / auth token may be stale, so reload instead of nudging a
  // dead <video>.
  if (const long at = suspend_boottime_ms_.load(); reload_after_suspend_ms_ > 0 && at > 0) {
    const long slept = boottime_ms() - at;
    if (slept >= reload_after_suspend_ms_) {
      const bool ok = client_.reload();
      info(ok ? std::format("player: slept {} ms (>= {} ms) — reloaded to refresh stream/token",
                            slept, reload_after_suspend_ms_)
              : "player: reload after long suspend failed (engine unreachable?)");
      return ok;
    }
  }

  const bool ok = client_.eval_void(play_js());
  info(ok ? "player: resume nudge sent" : "player: resume nudge failed (engine unreachable?)");
  return ok;
}

}  // namespace deckback
