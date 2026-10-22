#pragma once
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace deckback {

// CLOCK_MONOTONIC in milliseconds. Does NOT advance across system suspend; durations that must
// span a suspend use CLOCK_BOOTTIME instead.
inline long mono_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1'000'000L;
}

// Test one bit in the unsigned-long array the EVIOCGBIT ioctls fill in.
inline bool test_bit(int bit, const unsigned long* arr) {
  return (arr[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1UL;
}

// ASCII-only lowercase copy, deliberately not locale-aware.
inline std::string ascii_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return out;
}

// Escape for a double-quoted JS string literal; the same escape set is valid for a JSON string.
// Control characters become \u00XX.
inline std::string js_string_escape(std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
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
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
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
        if (static_cast<unsigned char>(c) < 0x20) {
          out += "\\u00";
          out.push_back(kHex[(c >> 4) & 0xF]);
          out.push_back(kHex[c & 0xF]);
        } else {
          out.push_back(c);
        }
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
