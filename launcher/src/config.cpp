#include "config.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

#include "log.hpp"

namespace deckback {
namespace {

std::optional<std::string> slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return std::nullopt;
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Find the position just after the colon following the top-level "key".
std::optional<size_t> value_pos(const std::string& s, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  size_t k = s.find(needle);
  if (k == std::string::npos) return std::nullopt;
  size_t colon = s.find(':', k + needle.size());
  if (colon == std::string::npos) return std::nullopt;
  size_t p = colon + 1;
  while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
  return p;
}

std::optional<std::string> read_string(const std::string& s, std::string_view key) {
  auto p = value_pos(s, key);
  if (!p || *p >= s.size() || s[*p] != '"') return std::nullopt;
  size_t start = *p + 1;
  std::string out;
  for (size_t i = start; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\\' && i + 1 < s.size()) {
      out.push_back(s[++i]);
    } else if (c == '"') {
      return out;
    } else {
      out.push_back(c);
    }
  }
  return std::nullopt;
}

std::optional<long> read_int(const std::string& s, std::string_view key) {
  auto p = value_pos(s, key);
  if (!p) return std::nullopt;
  try {
    return std::stol(s.substr(*p));
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<bool> read_bool(const std::string& s, std::string_view key) {
  auto p = value_pos(s, key);
  if (!p) return std::nullopt;
  if (s.compare(*p, 4, "true") == 0) return true;
  if (s.compare(*p, 5, "false") == 0) return false;
  return std::nullopt;
}

std::vector<std::string> read_string_array(const std::string& s, std::string_view key) {
  std::vector<std::string> out;
  auto p = value_pos(s, key);
  if (!p || *p >= s.size() || s[*p] != '[') return out;
  size_t end = s.find(']', *p);
  if (end == std::string::npos) return out;
  const std::string body = s.substr(*p + 1, end - *p - 1);
  for (size_t i = 0; i < body.size(); ++i) {
    if (body[i] != '"') continue;
    std::string item;
    for (size_t j = i + 1; j < body.size(); ++j) {
      if (body[j] == '\\' && j + 1 < body.size()) {
        item.push_back(body[++j]);
      } else if (body[j] == '"') {
        i = j;
        break;
      } else {
        item.push_back(body[j]);
      }
    }
    out.push_back(std::move(item));
  }
  return out;
}

// Reads a flat top-level object of "key": "value" string pairs (app.json's `keymap`). Keys
// beginning with '$' are treated as documentation (`$comment`) and skipped. Order is preserved so
// warnings and logs list bindings the way the user wrote them. Nested objects/arrays inside the
// value are not supported — the keymap is deliberately flat.
std::vector<std::pair<std::string, std::string>> read_string_object(const std::string& s,
                                                                    std::string_view key) {
  std::vector<std::pair<std::string, std::string>> out;
  auto p = value_pos(s, key);
  if (!p || *p >= s.size() || s[*p] != '{') return out;

  // Find the matching close brace so a later top-level key cannot leak in.
  int depth = 0;
  size_t end = std::string::npos;
  for (size_t i = *p; i < s.size(); ++i) {
    if (s[i] == '{')
      ++depth;
    else if (s[i] == '}' && --depth == 0) {
      end = i;
      break;
    }
  }
  if (end == std::string::npos) return out;

  // Scan "name" : "value" pairs inside the body.
  std::vector<std::string> tokens;
  for (size_t i = *p + 1; i < end; ++i) {
    if (s[i] != '"') continue;
    std::string tok;
    size_t j = i + 1;
    for (; j < end; ++j) {
      if (s[j] == '\\' && j + 1 < end)
        tok.push_back(s[++j]);
      else if (s[j] == '"')
        break;
      else
        tok.push_back(s[j]);
    }
    i = j;
    tokens.push_back(std::move(tok));
  }
  for (size_t i = 0; i + 1 < tokens.size(); i += 2) {
    if (!tokens[i].empty() && tokens[i][0] == '$') continue;  // $comment
    out.emplace_back(tokens[i], tokens[i + 1]);
  }
  return out;
}

// Typed field assignment: overwrite `out` only when the key is present and parses. Overloaded on
// the field type so Config::load stays one line per key with no cast noise.
void read_into(const std::string& s, std::string_view key, std::string& out) {
  if (auto v = read_string(s, key)) out = std::move(*v);
}
void read_into(const std::string& s, std::string_view key, bool& out) {
  if (auto v = read_bool(s, key)) out = *v;
}
void read_into(const std::string& s, std::string_view key, int& out) {
  if (auto v = read_int(s, key)) out = static_cast<int>(*v);
}
void read_into(const std::string& s, std::string_view key, long& out) {
  if (auto v = read_int(s, key)) out = *v;
}

}  // namespace

std::optional<Config> Config::load(const std::string& path) {
  auto text = slurp(path);
  if (!text) {
    error("config: cannot read " + path);
    return std::nullopt;
  }
  const std::string& s = *text;
  Config c;
  read_into(s, "url", c.url);
  read_into(s, "user_agent", c.user_agent);
  read_into(s, "remote_debugging_port", c.remote_debugging_port);
  read_into(s, "restart_on_crash", c.watchdog_restart_on_crash);
  read_into(s, "max_restarts_per_minute", c.watchdog_max_restarts_per_minute);
  c.cobalt_flags = read_string_array(s, "cobalt_flags");
  // `"keymap"` includes its closing quote in the needle, so it cannot match `"keymap_player"`.
  c.keymap = read_string_object(s, "keymap");
  c.keymap_player = read_string_object(s, "keymap_player");
  c.keymap_osk = read_string_object(s, "keymap_osk");
  c.keymap_lt = read_string_object(s, "keymap_lt");
  c.keymap_rt = read_string_object(s, "keymap_rt");
  read_into(s, "disable_touch", c.disable_touch);
  read_into(s, "block_touchscreen", c.block_touchscreen);
  read_into(s, "touch_lock_enabled", c.touch_lock_enabled);
  read_into(s, "touch_lock_chord", c.touch_lock_chord);
  read_into(s, "touch_lock_unlock_hold_ms", c.touch_lock_unlock_hold_ms);
  read_into(s, "touch_lock_toast", c.touch_lock_toast);
  read_into(s, "touch_lock_haptic", c.touch_lock_haptic);
  read_into(s, "first_run_overlay", c.first_run_overlay);
  read_into(s, "right_stick_scroll", c.right_stick_scroll);
  read_into(s, "right_stick_deadzone", c.right_stick_deadzone);
  read_into(s, "right_stick_slow_ms", c.right_stick_slow_ms);
  read_into(s, "right_stick_fast_ms", c.right_stick_fast_ms);
  read_into(s, "voice_enabled", c.voice_enabled);
  read_into(s, "voice_hold_ms", c.voice_hold_ms);
  read_into(s, "voice_duck", c.voice_duck);
  read_into(s, "voice_click_toggles", c.voice_click_toggles);
  c.voice_mic_selectors = read_string_array(s, "voice_mic_selectors");
  read_into(s, "steer_av1_unsupported", c.steer_av1);
  read_into(s, "mic_autogrant", c.mic_autogrant);
  read_into(s, "error_page", c.error_page);
  read_into(s, "error_retry_min_ms", c.error_retry_min_ms);
  read_into(s, "error_retry_max_ms", c.error_retry_max_ms);
  read_into(s, "error_title", c.error_title);
  read_into(s, "error_hint", c.error_hint);
  read_into(s, "cdm_url", c.cdm_url);
  read_into(s, "cdm_sha256", c.cdm_sha256);
  read_into(s, "log_directory", c.log_dir);
  read_into(s, "log_max_bytes", c.log_max_bytes);
  read_into(s, "log_max_files", c.log_max_files);
  read_into(s, "log_mirror_stderr", c.log_to_stderr);
  read_into(s, "devtools_poll_ms", c.devtools_poll_ms);
  read_into(s, "idle_inhibit_synthetic_fallback", c.idle_inhibit_synthetic_fallback);
  read_into(s, "resume_probe_host", c.resume_probe_host);
  read_into(s, "resume_probe_port", c.resume_probe_port);
  read_into(s, "resume_online_timeout_ms", c.resume_online_timeout_ms);
  read_into(s, "resume_reload_after_ms", c.resume_reload_after_ms);
  return c;
}

}  // namespace deckback
