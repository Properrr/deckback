#pragma once
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace deckback {

// CLOCK_MONOTONIC in milliseconds. The launcher's timers all use this clock: it never jumps with
// wall-clock changes. Note it does NOT advance across system suspend — durations that must span a
// suspend (player.cpp's reload-after-sleep) use CLOCK_BOOTTIME instead.
inline long mono_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1'000'000L;
}

// Test one bit in the unsigned-long array the EVIOCGBIT ioctls fill in.
inline bool test_bit(int bit, const unsigned long* arr) {
  return (arr[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1UL;
}

// ASCII-only lowercase copy. Deliberately not locale-aware: every caller compares against fixed
// ASCII tokens (HTTP header names, evdev device names, WM_CLASS values).
inline std::string ascii_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return out;
}

// Escape for embedding in a double-quoted JS string literal — which is the same escape set a JSON
// string needs, so the CDP client uses it for JSON params too. Handles the characters our generated
// text can contain; exotic control characters never appear in it.
inline std::string js_string_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

// Split on runs of spaces/tabs, dropping empty tokens (DECKBACK_EXTRA_ARGS parsing).
inline std::vector<std::string> split_whitespace(std::string_view sv) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p < sv.size()) {
    size_t b = sv.find_first_not_of(" \t", p);
    if (b == std::string_view::npos) break;
    size_t e = sv.find_first_of(" \t", b);
    out.emplace_back(sv.substr(b, e == std::string_view::npos ? e : e - b));
    if (e == std::string_view::npos) break;
    p = e;
  }
  return out;
}

}  // namespace deckback
