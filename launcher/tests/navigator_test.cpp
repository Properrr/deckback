// app_needle() is the predicate the Navigator uses to decide "are we already on the TV app?" —
// a needle that matched about:blank would stop the initial navigation from ever firing, and one
// that failed to match the loaded ".../tv#/" would re-navigate in a loop. Pin it.

#include "navigator.hpp"

#include <cassert>
#include <cstdio>
#include <string>

using namespace deckback;

static void test_app_needle() {
  // The shipped URL: scheme stripped, trailing slash irrelevant.
  assert(app_needle("https://www.youtube.com/tv") == "www.youtube.com/tv");
  assert(app_needle("https://www.youtube.com/tv/") == "www.youtube.com/tv");

  // Query/fragment never belong in the needle: the loaded page grows a "#/" route.
  assert(app_needle("https://www.youtube.com/tv?env=prod") == "www.youtube.com/tv");
  assert(app_needle("https://www.youtube.com/tv#?v=abc") == "www.youtube.com/tv");

  // No scheme is tolerated (a hand-edited config).
  assert(app_needle("www.youtube.com/tv") == "www.youtube.com/tv");

  // Degenerate inputs collapse to empty — the loop treats an empty needle as "never on the app",
  // which keeps navigating rather than falsely declaring success.
  assert(app_needle("").empty());
  assert(app_needle("https:///").empty());
}

static void test_needle_matches_loaded_locations() {
  const std::string needle = app_needle("https://www.youtube.com/tv");
  // What the engine actually reports once Leanback is up.
  assert(std::string("https://www.youtube.com/tv#/").find(needle) != std::string::npos);
  assert(std::string("https://www.youtube.com/tv#/watch?v=abc").find(needle) != std::string::npos);
  // What it reports when we are NOT on the app.
  assert(std::string("about:blank").find(needle) == std::string::npos);
  assert(std::string("https://www.youtube.com/?app=desktop").find(needle) == std::string::npos);
}

int main() {
  test_app_needle();
  test_needle_matches_loaded_locations();
  std::puts("navigator_test: OK");
  return 0;
}
