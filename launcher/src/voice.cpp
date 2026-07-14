#include "voice.hpp"

#include <cstdlib>
#include <format>

#include "log.hpp"
#include "scripts.hpp"

namespace deckback {
namespace {

// Duck/restore now live in config/scripts/voice_*.js (ScriptLibrary). `pause` is the default
// because it is the only one that removes speaker bleed entirely; `mute` keeps the timeline moving,
// which some users prefer. Each script carries a `/*voice*/` marker so the test double can tell it
// apart from player_pause.js (both call pause()).
std::string voice_duck_pause_js() { return ScriptLibrary::instance().render("voice_pause"); }
std::string voice_duck_play_js() { return ScriptLibrary::instance().render("voice_play"); }
std::string voice_duck_mute_js() { return ScriptLibrary::instance().render("voice_mute"); }
std::string voice_duck_unmute_js() { return ScriptLibrary::instance().render("voice_unmute"); }

}  // namespace

// ---- pure helpers ------------------------------------------------------------------------------

std::optional<DuckMode> parse_duck_mode(std::string_view value) {
  if (value == "none") return DuckMode::None;
  if (value == "mute") return DuckMode::Mute;
  if (value == "pause") return DuckMode::Pause;
  return std::nullopt;
}

const char* duck_mode_name(DuckMode m) {
  switch (m) {
    case DuckMode::None:
      return "none";
    case DuckMode::Mute:
      return "mute";
    case DuckMode::Pause:
      return "pause";
  }
  return "none";
}

std::optional<std::pair<double, double>> parse_point(std::string_view s) {
  const size_t comma = s.find(',');
  if (comma == std::string_view::npos || comma == 0 || comma + 1 >= s.size()) return std::nullopt;
  const std::string xs(s.substr(0, comma));
  const std::string ys(s.substr(comma + 1));
  char* end = nullptr;
  const double x = std::strtod(xs.c_str(), &end);
  if (end != xs.c_str() + xs.size()) return std::nullopt;
  const double y = std::strtod(ys.c_str(), &end);
  if (end != ys.c_str() + ys.size()) return std::nullopt;
  return std::pair<double, double>{x, y};
}

std::string mic_probe_js(const std::vector<std::string>& selectors) {
  // config/scripts/mic_probe.js, rendered with the selector list as a JSON string[] param (one
  // central escaper — no more per-call js_quote). Returns "x,y" for the first visible candidate, ""
  // when nothing matches.
  return ScriptLibrary::instance().render("mic_probe", ScriptParams().set("selectors", selectors));
}

HoldToTalk::Action HoldToTalk::on_press(long now_ms) {
  if (down_) return Action::None;  // kernel auto-repeat, or a duplicate from a merged device
  down_ = true;
  press_ms_ = now_ms;
  // Deliberately does NOT start here. Starting on the press edge would make every stray tap open
  // the mic — the debounce is the whole reason this class exists.
  return Action::None;
}

HoldToTalk::Action HoldToTalk::on_tick(long now_ms) {
  if (!down_ || active_) return Action::None;
  if (now_ms < deadline_ms()) return Action::None;
  active_ = true;
  return Action::Start;
}

HoldToTalk::Action HoldToTalk::on_release(long now_ms) {
  (void)now_ms;
  if (!down_) return Action::None;
  down_ = false;
  if (!active_) return Action::None;  // a tap shorter than hold_ms: the mic never opened
  active_ = false;
  return Action::Stop;
}

// ---- the controller ----------------------------------------------------------------------------

VoiceController::VoiceController(std::string host, int port, VoiceConfig cfg)
    : client_(std::move(host), port), cfg_(std::move(cfg)) {
  probe_js_ = mic_probe_js(cfg_.mic_selectors);
  info(std::format("voice: hold {} ms, duck={}, {} mic selector(s), click={}", cfg_.hold_ms,
                   duck_mode_name(cfg_.duck), cfg_.mic_selectors.size(),
                   cfg_.click_toggles ? "toggle" : "hold"));
}

VoiceController::Located VoiceController::locate_button() {
  auto reply = client_.eval_string(probe_js_);
  if (!reply) return {Located::Unreachable, {}};
  if (reply->empty()) return {Located::NoButton, {}};
  auto pt = parse_point(*reply);
  // A malformed reply is treated as absent. Never fall back to (0,0): clicking the viewport corner
  // because we could not find the mic is the dead-button failure wearing a different hat.
  if (!pt) return {Located::NoButton, {}};
  return {Located::Found, *pt};
}

void VoiceController::duck() {
  switch (cfg_.duck) {
    case DuckMode::None:
      return;
    case DuckMode::Pause:
      ducked_ = client_.eval_bool(voice_duck_pause_js()).value_or(false);
      return;
    case DuckMode::Mute:
      ducked_ = client_.eval_bool(voice_duck_mute_js()).value_or(false);
      return;
  }
}

void VoiceController::unduck() {
  if (!ducked_) return;  // we did not change it, so we must not "restore" it
  ducked_ = false;
  switch (cfg_.duck) {
    case DuckMode::None:
      return;
    case DuckMode::Pause:
      client_.eval_void(voice_duck_play_js());
      return;
    case DuckMode::Mute:
      client_.eval_void(voice_duck_unmute_js());
      return;
  }
}

bool VoiceController::start() {
  if (listening_) return true;
  const Located found = locate_button();
  if (found.status == Located::Unreachable) {
    warn("voice: cannot reach the engine over CDP — mic not opened");
    return false;
  }
  if (found.status == Located::NoButton) {
    // The V0 failure. Say it once, plainly, and change nothing else — no dead button, no ducked
    // audio the user cannot un-duck.
    if (!warned_no_button_) {
      warn(
          "voice: no soft-mic button found on the page. Voice search is NOT available. This is the "
          "expected result if Leanback hides the web mic control under our Cobalt UA (findings "
          "input-ux §13.2) — it is not a crash. Update voice_mic_selectors in app.json if the "
          "control moved.");
      warned_no_button_ = true;
    }
    return false;
  }
  button_ = found.point;

  // Duck BEFORE opening the mic: the speakers are ~15 cm from the mic array, and the first word is
  // the one ASR needs most.
  duck();

  const bool ok = cfg_.click_toggles ? client_.mouse_click(button_.first, button_.second)
                                     : client_.mouse_press(button_.first, button_.second);
  if (!ok) {
    warn("voice: mic button click was rejected by the engine");
    unduck();
    return false;
  }
  listening_ = true;
  info(
      std::format("voice: listening (mic button at {:.0f},{:.0f})", button_.first, button_.second));
  return true;
}

void VoiceController::stop() {
  if (!listening_) return;
  listening_ = false;
  // A tap-to-toggle control needs a second click to close; a press-and-hold control needs the
  // release of the press we are still holding.
  if (cfg_.click_toggles)
    client_.mouse_click(button_.first, button_.second);
  else
    client_.mouse_release(button_.first, button_.second);
  unduck();
  info("voice: stopped");
}

}  // namespace deckback
