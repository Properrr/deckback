// Everything that drives the fake CDP endpoint over a real loopback socket.
// Linked as one binary; CTest runs each case in its own process (see harness.hpp).
#include "harness.hpp"

DECKBACK_TEST_DECL(devtools);
DECKBACK_TEST_DECL(navigator);
DECKBACK_TEST_DECL(player);
DECKBACK_TEST_DECL(errorpage);
DECKBACK_TEST_DECL(gestures);

int main(int argc, char** argv) {
  static const DeckbackTestCase kCases[] = {
      {"devtools", devtools_test_main}, {"navigator", navigator_test_main},
      {"player", player_test_main},     {"errorpage", errorpage_test_main},
      {"gestures", gestures_test_main},
  };
  return DECKBACK_RUN_GROUP(kCases);
}
