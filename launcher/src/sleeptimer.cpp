#include "sleeptimer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>

#include "json.hpp"

namespace deckback {
namespace {

// A ladder entry longer than this is a typo in a hot-swapped config, not a choice: the timer holds
// no state across a launcher restart, so a 30-hour countdown could never be reached anyway.
constexpr double kMaxOptionMinutes = 24 * 60;

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

// Shortest round-tripping text for a minute count: 15 -> "15", 0.5 -> "0.5". This is the combo's
// stored value, so it must parse back to the same double the ladder holds or a selection would not
// match any entry.
std::string minutes_value(double m) { return std::format("{}", m); }

}  // namespace

std::vector<double> default_sleep_options() { return {5, 15, 30, 45, 60, 90, 120}; }

std::vector<double> parse_sleep_options(std::string_view csv) {
  std::vector<double> out;
  size_t pos = 0;
  while (pos <= csv.size()) {
    const size_t comma = csv.find(',', pos);
    const std::string_view tok = trim(
        csv.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos));
    if (!tok.empty()) {
      const std::string owned(tok);
      char* end = nullptr;
      const double v = std::strtod(owned.c_str(), &end);
      // Reject trailing junk ("15min") rather than silently accepting the 15: a config typo that
      // half-works is how a hot-swap ships a ladder nobody intended.
      if (end && *end == '\0' && std::isfinite(v) && v > 0 && v <= kMaxOptionMinutes)
        out.push_back(v);
    }
    if (comma == std::string_view::npos) break;
    pos = comma + 1;
  }
  // Ascending and deduped: the menu is a duration ladder, and ←/→ walking it in authoring order
  // would let a hot-swap produce "45 min, 5 min, 30 min".
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::string format_sleep_minutes(double minutes) {
  if (minutes <= 0) return "Off";
  if (minutes < 1) {
    const long secs = std::lround(minutes * 60);
    return std::format("{} s", secs);
  }
  const long total = std::lround(minutes);
  const long h = total / 60;
  const long m = total % 60;
  if (h == 0) return std::format("{} min", m);
  if (m == 0) return std::format("{} h", h);
  return std::format("{} h {} min", h, m);
}

std::string format_remaining(long ms) {
  if (ms <= 0) return "0 s";
  // Under a minute, count seconds: "1 min" frozen on screen for the last 59 seconds reads as a
  // stuck timer, which is exactly when the user is deciding whether to reach for the Deck.
  if (ms < 60'000) return std::format("{} s", (ms + 999) / 1000);
  // Round UP so a freshly armed 15-minute timer says "15 min", not "14 min".
  const long total = (ms + 59'999) / 60'000;
  const long h = total / 60;
  const long m = total % 60;
  if (h == 0) return std::format("{} min", m);
  if (m == 0) return std::format("{} h", h);
  return std::format("{} h {} min", h, m);
}

SleepTimer::SleepTimer(std::vector<double> options_minutes, int warn_seconds)
    : options_(std::move(options_minutes)), warn_seconds_(warn_seconds < 0 ? 0 : warn_seconds) {
  if (options_.empty()) options_ = default_sleep_options();
}

void SleepTimer::arm(double minutes, long now_ms) {
  if (!(minutes > 0)) {
    disarm();
    return;
  }
  armed_ = true;
  minutes_ = minutes;
  deadline_ms_ = now_ms + static_cast<long>(minutes * 60'000.0);
  warned_ = false;
}

void SleepTimer::disarm() {
  armed_ = false;
  minutes_ = 0;
  deadline_ms_ = 0;
  warned_ = false;
}

SleepTimer::Tick SleepTimer::tick(long now_ms) {
  if (!armed_) return Tick::None;
  if (now_ms >= deadline_ms_) {
    // Disarm on the way out: the caller pauses the video, and a timer that stayed armed would
    // re-pause it every tick, fighting the user who presses play again.
    disarm();
    return Tick::Fire;
  }
  if (!warned_ && warn_seconds_ > 0) {
    const long lead = static_cast<long>(warn_seconds_) * 1000L;
    // No warning for a duration that is not longer than the lead: it would fire in the same breath
    // as the arming and tell the user nothing they did not just type.
    if (minutes_ * 60'000.0 > static_cast<double>(lead) && deadline_ms_ - now_ms <= lead) {
      warned_ = true;
      return Tick::Warn;
    }
  }
  return Tick::None;
}

long SleepTimer::remaining_ms(long now_ms) const {
  if (!armed_) return 0;
  const long left = deadline_ms_ - now_ms;
  return left > 0 ? left : 0;
}

std::string SleepTimer::status_line(long now_ms) const {
  if (!armed_) return "Off";
  return "Stops in " + format_remaining(remaining_ms(now_ms));
}

std::vector<SleepOption> SleepTimer::options() const {
  std::vector<SleepOption> out;
  out.reserve(options_.size() + 1);
  out.push_back({"0", "Off"});
  for (double m : options_) out.push_back({minutes_value(m), format_sleep_minutes(m)});
  return out;
}

std::string SleepTimer::osd_model_json(long now_ms) const {
  using json::Value;
  std::vector<Value> opts;
  for (const SleepOption& o : options())
    opts.push_back(Value::object({{"value", Value(o.value)}, {"label", Value(o.label)}}));

  Value row = Value::object({
      {"key", Value(std::string("timer"))},
      {"label", Value(std::string("Stop playback in"))},
      {"kind", Value(std::string("combo"))},
      {"value", Value(armed_ ? minutes_value(minutes_) : std::string("0"))},
      {"options", Value::array(std::move(opts))},
  });

  std::vector<Value> rows;
  rows.push_back(std::move(row));
  Value model = Value::object({
      {"ns", Value(std::string("sleep"))},
      {"status", Value(status_line(now_ms))},
      {"note", Value(std::string("When the timer runs out playback pauses, and the Deck is free to "
                                 "dim and sleep on its own."))},
      {"rows", Value::array(std::move(rows))},
  });
  return json::dump(model, -1);
}

bool SleepTimer::apply_action(std::string_view action, long now_ms) {
  constexpr std::string_view kPrefix = "sleep.";
  if (action.substr(0, kPrefix.size()) != kPrefix) return false;
  action.remove_prefix(kPrefix.size());
  const size_t eq = action.find('=');
  if (eq == std::string_view::npos) return false;
  const std::string_view key = action.substr(0, eq);
  const std::string_view val = action.substr(eq + 1);
  if (key != "timer") return false;

  const std::string owned(val);
  char* end = nullptr;
  const double minutes = std::strtod(owned.c_str(), &end);
  if (!end || *end != '\0' || !std::isfinite(minutes)) return false;
  if (minutes <= 0) {
    if (!armed_) return false;
    disarm();
    return true;
  }
  // Only values the launcher itself offered: the OSD echoes back a value from the ladder we sent
  // it, so anything else is a stale menu from before a config hot-swap, not a user choice.
  const bool known =
      std::any_of(options_.begin(), options_.end(), [minutes](double m) { return m == minutes; });
  if (!known) return false;
  arm(minutes, now_ms);
  return true;
}

}  // namespace deckback
