#include "config.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "fileio.hpp"
#include "json.hpp"
#include "log.hpp"

namespace deckback {
namespace {

// The schema: one row per setting, binding an exact dotted JSON path to the Config member it fills.
// The tables are also the list of known keys used for unknown-key reporting.

template <typename T>
struct Field {
  std::string_view path;
  T Config::* member;
};

// Inclusive bounds. Out-of-range values are clamped with a warning rather than rejected.
struct Range {
  long min;
  long max;
};

constexpr Field<std::string> kStringFields[] = {
    {"url", &Config::url},
    {"user_agent", &Config::user_agent},
    {"error_title", &Config::error_title},
    {"error_hint", &Config::error_hint},
    {"cdm_url", &Config::cdm_url},
    {"cdm_sha256", &Config::cdm_sha256},
    {"log.log_directory", &Config::log_dir},
    {"power.resume_probe_host", &Config::resume_probe_host},
    {"caption_language", &Config::caption_language},
    {"caption_control", &Config::caption_control},
    {"caption_type", &Config::caption_type},
};

constexpr Field<bool> kBoolFields[] = {
    {"watchdog.restart_on_crash", &Config::watchdog_restart_on_crash},
    {"disable_touch", &Config::disable_touch},
    {"touch_gestures", &Config::touch_gestures},
    {"touch_hide_cursor", &Config::touch_hide_cursor},
    {"first_run_overlay", &Config::first_run_overlay},
    {"captions_toast", &Config::captions_toast},
    {"caption_remember", &Config::caption_remember},
    {"caption_on", &Config::caption_on},
    {"right_stick_scroll", &Config::right_stick_scroll},
    {"quality.steer_av1_unsupported", &Config::steer_av1},
    {"mic_autogrant", &Config::mic_autogrant},
    {"error_page", &Config::error_page},
    {"log.log_mirror_stderr", &Config::log_to_stderr},
    {"power.keep_awake_warn", &Config::keep_awake_warn},
};

struct IntField {
  std::string_view path;
  int Config::* member;
  Range range;
};

constexpr IntField kIntFields[] = {
    {"remote_debugging_port", &Config::remote_debugging_port, {0, 65535}},
    {"watchdog.max_restarts_per_minute", &Config::watchdog_max_restarts_per_minute, {1, 1000}},
    // The right stick's raw evdev range is ±32767; a deadzone at or past that kills the feature.
    {"right_stick_deadzone", &Config::right_stick_deadzone, {1, 32'000}},
    {"right_stick_slow_ms", &Config::right_stick_slow_ms, {10, 5'000}},
    {"right_stick_fast_ms", &Config::right_stick_fast_ms, {10, 5'000}},
    {"skip_seconds", &Config::skip_seconds, {1, 600}},
    // The poll is the touch input latency; 0 would busy-loop the poll thread against the engine.
    {"touch_poll_ms", &Config::touch_poll_ms, {10, 1'000}},
    // Below ~20 px a drag fires arrows faster than Leanback can move focus; above the panel width
    // it never fires.
    {"touch_step_px", &Config::touch_step_px, {20, 1'280}},
    {"touch_max_steps", &Config::touch_max_steps, {1, 50}},
    {"touch_edge_px", &Config::touch_edge_px, {4, 400}},
    {"touch_long_press_ms", &Config::touch_long_press_ms, {150, 5'000}},
    {"touch_double_tap_ms", &Config::touch_double_tap_ms, {80, 2'000}},
    {"error_retry_min_ms", &Config::error_retry_min_ms, {100, 600'000}},
    {"error_retry_max_ms", &Config::error_retry_max_ms, {100, 600'000}},
    {"log.log_max_files", &Config::log_max_files, {0, 100}},
    // 0 would busy-loop the poll thread against the engine.
    {"power.devtools_poll_ms", &Config::devtools_poll_ms, {100, 60'000}},
    {"power.resume_probe_port", &Config::resume_probe_port, {1, 65535}},
    {"power.resume_online_timeout_ms", &Config::resume_online_timeout_ms, {0, 120'000}},
    {"power.resume_reload_after_ms", &Config::resume_reload_after_ms, {0, 86'400'000}},
};

struct LongField {
  std::string_view path;
  long Config::* member;
  Range range;
};

constexpr LongField kLongFields[] = {
    {"log.log_max_bytes", &Config::log_max_bytes, {0, 1'000'000'000}},
};

struct RealField {
  std::string_view path;
  double Config::* member;
  double min, max;
};

// hold_scrub.js snaps to the nearest rate the player advertises; these bounds only keep a typo
// from producing a stalled or inaudible video.
constexpr RealField kRealFields[] = {
    {"touch_hold_rate", &Config::touch_hold_rate, 0.1, 4.0},
    {"touch_hold_slow_rate", &Config::touch_hold_slow_rate, 0.1, 4.0},
};

constexpr std::string_view kKeymapFields[] = {"keymap", "keymap_player", "keymap_osk", "keymap_lt",
                                              "keymap_rt"};

// Keys that are read outside the tables above, or that exist for humans/compatibility. Listed so
// the unknown-key check does not report them.
constexpr std::string_view kKnownExtraPaths[] = {
    "schema_version",
    "cobalt_flags",
    "caption_languages",
    "power.self_update_mode",
    "self_update_mode",  // accepted at top level too
    "self_update",       // deprecated legacy boolean
    // Removed keys, still accepted so older app.json files don't warn.
    "power.idle_inhibit_synthetic_fallback",
    "block_touchscreen",
    "touch_lock_enabled",
    "touch_lock_chord",
    "touch_lock_unlock_hold_ms",
    "touch_lock_toast",
    "touch_lock_haptic",
};

// Sections the launcher deliberately does not consume (documentation for humans and the engine's GN
// args); not reported as unknown.
constexpr std::string_view kIgnoredPrefixes[] = {
    "quality.max_height",
    "quality.prefer_codecs",
    "render.",
};

// The config schema this launcher understands. A file declaring a newer version is refused.
constexpr int kSupportedSchemaVersion = 1;

bool is_comment_key(std::string_view leaf) { return !leaf.empty() && leaf[0] == '$'; }

bool any_segment_is_comment(std::string_view path) {
  size_t pos = 0;
  while (pos <= path.size()) {
    const size_t dot = path.find('.', pos);
    const std::string_view seg =
        path.substr(pos, dot == std::string_view::npos ? std::string_view::npos : dot - pos);
    if (is_comment_key(seg)) return true;
    if (dot == std::string_view::npos) break;
    pos = dot + 1;
  }
  return false;
}

// Typed reads: an absent key keeps the default silently; a present key of the wrong type warns and
// keeps the default.
void bind_string(const json::Value& root, std::string_view path, std::string& out) {
  const json::Value* v = root.find(path);
  if (!v) return;
  if (const std::string* s = v->as_string()) {
    out = *s;
    return;
  }
  warn(std::string("config: '") + std::string(path) + "' must be a string — keeping the default");
}

void bind_bool(const json::Value& root, std::string_view path, bool& out) {
  const json::Value* v = root.find(path);
  if (!v) return;
  if (auto b = v->as_bool()) {
    out = *b;
    return;
  }
  warn(std::string("config: '") + std::string(path) +
       "' must be true or false — keeping the "
       "default");
}

std::optional<long> bind_number(const json::Value& root, std::string_view path, Range r) {
  const json::Value* v = root.find(path);
  if (!v) return std::nullopt;
  auto n = v->as_number();
  if (!n) {
    warn(std::string("config: '") + std::string(path) + "' must be a number — keeping the default");
    return std::nullopt;
  }
  if (!std::isfinite(*n) || std::trunc(*n) != *n) {
    warn(std::string("config: '") + std::string(path) +
         "' must be an integer — keeping the default");
    return std::nullopt;
  }
  if (*n < r.min || *n > r.max) {
    const long clamped = *n < r.min ? r.min : r.max;
    warn("config: '" + std::string(path) + "' = " + std::to_string(*n) + " is outside " +
         std::to_string(r.min) + ".." + std::to_string(r.max) + " — clamped to " +
         std::to_string(clamped));
    return clamped;
  }
  return static_cast<long>(*n);
}

// Like bind_number, but accepts non-integers.
std::optional<double> bind_real(const json::Value& root, std::string_view path, double min,
                                double max) {
  const json::Value* v = root.find(path);
  if (!v) return std::nullopt;
  auto n = v->as_number();
  if (!n) {
    warn(std::string("config: '") + std::string(path) + "' must be a number — keeping the default");
    return std::nullopt;
  }
  if (!std::isfinite(*n)) {
    warn(std::string("config: '") + std::string(path) + "' must be finite — keeping the default");
    return std::nullopt;
  }
  if (*n < min || *n > max) {
    const double clamped = *n < min ? min : max;
    warn("config: '" + std::string(path) + "' = " + std::format("{:g}", *n) + " is outside " +
         std::format("{:g}", min) + ".." + std::format("{:g}", max) + " — clamped to " +
         std::format("{:g}", clamped));
    return clamped;
  }
  return *n;
}

std::vector<std::string> bind_string_array(const json::Value& root, std::string_view path) {
  std::vector<std::string> out;
  const json::Value* v = root.find(path);
  if (!v) return out;
  const auto* arr = v->as_array();
  if (!arr) {
    warn(std::string("config: '") + std::string(path) + "' must be an array — ignoring it");
    return out;
  }
  for (const json::Value& item : *arr) {
    if (const std::string* s = item.as_string())
      out.push_back(*s);
    else
      warn(std::string("config: '") + std::string(path) +
           "' contains a non-string entry — skipping it");
  }
  return out;
}

// A flat object of "key": "value" string pairs, in written order. `$`-prefixed keys are
// documentation (`$comment`) and skipped. Nested values are not supported.
std::vector<std::pair<std::string, std::string>> bind_string_object(const json::Value& root,
                                                                    std::string_view path) {
  std::vector<std::pair<std::string, std::string>> out;
  const json::Value* v = root.find(path);
  if (!v) return out;
  const auto* obj = v->as_object();
  if (!obj) {
    warn(std::string("config: '") + std::string(path) + "' must be an object — ignoring it");
    return out;
  }
  for (const json::Member& m : *obj) {
    if (is_comment_key(m.first)) continue;
    if (const std::string* s = m.second.as_string())
      out.emplace_back(m.first, *s);
    else
      warn("config: '" + std::string(path) + "." + m.first + "' must be a string — skipping it");
  }
  return out;
}

bool path_is_known(std::string_view path) {
  for (const auto& f : kStringFields)
    if (f.path == path) return true;
  for (const auto& f : kBoolFields)
    if (f.path == path) return true;
  for (const auto& f : kIntFields)
    if (f.path == path) return true;
  for (const auto& f : kLongFields)
    if (f.path == path) return true;
  for (const auto& f : kRealFields)
    if (f.path == path) return true;
  for (std::string_view p : kKnownExtraPaths)
    if (p == path) return true;
  for (std::string_view k : kKeymapFields)
    if (path == k || path.starts_with(std::string(k) + ".")) return true;
  for (std::string_view p : kIgnoredPrefixes)
    if (path == p || path.starts_with(p)) return true;
  return false;
}

// Warns about every leaf path no table knows (`$`-prefixed segments excluded).
void report_unknown_keys(const json::Value& root) {
  for (const std::string& p : root.leaf_paths()) {
    if (any_segment_is_comment(p)) continue;
    if (path_is_known(p)) continue;
    warn("config: unknown key '" + p + "' — ignored (typo? or written for a newer launcher?)");
  }
}

}  // namespace

SelfUpdateMode parse_self_update_mode(std::string_view s) {
  if (s == "off") return SelfUpdateMode::Off;
  if (s == "auto") return SelfUpdateMode::Auto;
  if (s != "notify")
    warn("config: self_update_mode '" + std::string(s) +
         "' unrecognised (off|notify|auto) — using notify");
  return SelfUpdateMode::Notify;  // never deploy without consent
}

const char* self_update_mode_name(SelfUpdateMode m) {
  switch (m) {
    case SelfUpdateMode::Off:
      return "off";
    case SelfUpdateMode::Notify:
      return "notify";
    case SelfUpdateMode::Auto:
      return "auto";
  }
  return "notify";
}

std::optional<Config> Config::load(const std::string& path) {
  auto text = read_file(path);
  if (!text) {
    error("config: cannot read " + path);
    return std::nullopt;
  }

  json::ParseResult parsed = json::parse(*text);
  if (!parsed.ok()) {
    error("config: " + path + ":" + std::to_string(parsed.error.line) + ": " +
          parsed.error.message + " (offset " + std::to_string(parsed.error.offset) + ")");
    return std::nullopt;
  }
  const json::Value& root = *parsed.value;
  if (!root.is_object()) {
    error("config: " + path + ": the top-level value must be a JSON object");
    return std::nullopt;
  }

  if (const json::Value* sv = root.find("schema_version")) {
    const auto n = sv->as_number();
    if (!n || !std::isfinite(*n) || std::trunc(*n) != *n || *n < 0) {
      error("config: " + path + ": schema_version must be a non-negative integer");
      return std::nullopt;
    }
    if (*n > kSupportedSchemaVersion) {
      error("config: " + path + ": schema_version " + std::to_string(*n) +
            " is newer than this launcher supports (" + std::to_string(kSupportedSchemaVersion) +
            ")");
      return std::nullopt;
    }
  }

  Config c;
  for (const auto& f : kStringFields) bind_string(root, f.path, c.*(f.member));
  for (const auto& f : kBoolFields) bind_bool(root, f.path, c.*(f.member));
  for (const auto& f : kIntFields)
    if (auto v = bind_number(root, f.path, f.range)) c.*(f.member) = static_cast<int>(*v);
  for (const auto& f : kLongFields)
    if (auto v = bind_number(root, f.path, f.range)) c.*(f.member) = *v;
  for (const auto& f : kRealFields)
    if (auto v = bind_real(root, f.path, f.min, f.max)) c.*(f.member) = *v;

  c.cobalt_flags = bind_string_array(root, "cobalt_flags");
  c.caption_languages = bind_string_array(root, "caption_languages");
  c.keymap = bind_string_object(root, "keymap");
  c.keymap_player = bind_string_object(root, "keymap_player");
  c.keymap_osk = bind_string_object(root, "keymap_osk");
  c.keymap_lt = bind_string_object(root, "keymap_lt");
  c.keymap_rt = bind_string_object(root, "keymap_rt");

  // The tri-state `self_update_mode` (under `power` or at top level) wins over the legacy
  // `self_update` boolean.
  const json::Value* mode = root.find("power.self_update_mode");
  if (!mode) mode = root.find("self_update_mode");
  if (mode && mode->as_string()) {
    c.self_update_mode = parse_self_update_mode(*mode->as_string());
  } else if (mode) {
    warn("config: 'self_update_mode' must be a string (off|notify|auto) — using notify");
  } else if (const json::Value* legacy = root.find("self_update")) {
    if (auto b = legacy->as_bool()) {
      c.self_update_mode = *b ? SelfUpdateMode::Auto : SelfUpdateMode::Off;
      warn("config: 'self_update' is deprecated — use 'self_update_mode' (off|notify|auto)");
    }
  }

  // A retry window whose floor exceeds its ceiling would make the backoff nonsense.
  if (c.error_retry_min_ms > c.error_retry_max_ms) {
    warn("config: error_retry_min_ms (" + std::to_string(c.error_retry_min_ms) +
         ") exceeds error_retry_max_ms (" + std::to_string(c.error_retry_max_ms) +
         ") — swapping them");
    std::swap(c.error_retry_min_ms, c.error_retry_max_ms);
  }
  // Likewise the scroll rate ramps from slow (at the deadzone edge) to fast (at full deflection).
  if (c.right_stick_fast_ms > c.right_stick_slow_ms) {
    warn("config: right_stick_fast_ms (" + std::to_string(c.right_stick_fast_ms) +
         ") exceeds right_stick_slow_ms (" + std::to_string(c.right_stick_slow_ms) +
         ") — swapping them");
    std::swap(c.right_stick_fast_ms, c.right_stick_slow_ms);
  }

  report_unknown_keys(root);
  return c;
}

}  // namespace deckback
