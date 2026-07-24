// Everything that drives the fake CDP endpoint over a real loopback socket.
//
// One binary for this group instead of one per file (see harness.hpp). CTest still registers each
// case by name and runs it in its own process, so both the reporting and the isolation are
// unchanged — only the link steps are fewer.
#include "harness.hpp"

DECKBACK_TEST_DECL(devtools);
DECKBACK_TEST_DECL(navigator);
DECKBACK_TEST_DECL(player);
DECKBACK_TEST_DECL(errorpage);

int main(int argc, char** argv) {
  static const DeckbackTestCase kCases[] = {
      {"devtools", devtools_test_main},
      {"navigator", navigator_test_main},
      {"player", player_test_main},
      {"errorpage", errorpage_test_main},
  };
  return DECKBACK_RUN_GROUP(kCases);
}
