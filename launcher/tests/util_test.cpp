// util.hpp: the shared header-only helpers. split_whitespace feeds DECKBACK_EXTRA_ARGS straight
// onto the engine command line, and js_string_escape guards every injected script — both fail as
// silent misbehaviour, not as errors, so they get pinned here.

#include "util.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace deckback;

static void test_split_whitespace() {
  assert(split_whitespace("").empty());
  assert(split_whitespace("   \t ").empty());

  const std::vector<std::string> one = split_whitespace("--no-sandbox");
  assert(one.size() == 1 && one[0] == "--no-sandbox");

  const std::vector<std::string> many = split_whitespace("  --a\t--b=1   --c  ");
  assert(many.size() == 3);
  assert(many[0] == "--a" && many[1] == "--b=1" && many[2] == "--c");
}

static void test_js_string_escape() {
  assert(js_string_escape("plain") == "plain");
  assert(js_string_escape("a\"b") == "a\\\"b");
  assert(js_string_escape("a\\b") == "a\\\\b");
  assert(js_string_escape("a\nb\rc\td") == "a\\nb\\rc\\td");
}

static void test_ascii_lower() {
  assert(ascii_lower("Content-Length: 42") == "content-length: 42");
  assert(ascii_lower("FTS3528") == "fts3528");
  // Non-ASCII bytes pass through untouched — this is not a locale-aware lowercase.
  assert(ascii_lower("\xC3\x84") == "\xC3\x84");
}

static void test_test_bit() {
  unsigned long arr[2] = {0, 0};
  arr[0] |= 1UL << 3;
  arr[1] |= 1UL << 1;
  assert(test_bit(3, arr));
  assert(!test_bit(4, arr));
  assert(test_bit(static_cast<int>(8 * sizeof(long)) + 1, arr));
}

static void test_mono_ms_monotonic() {
  const long a = mono_ms();
  const long b = mono_ms();
  assert(a > 0 && b >= a);
}

int main() {
  test_split_whitespace();
  test_js_string_escape();
  test_ascii_lower();
  test_test_bit();
  test_mono_ms_monotonic();
  std::puts("util_test: OK");
  return 0;
}
