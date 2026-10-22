// Process, platform and network-facing subsystems.
// Linked as one binary; CTest runs each case in its own process (see harness.hpp).
#include "harness.hpp"

DECKBACK_TEST_DECL(watchdog);
DECKBACK_TEST_DECL(updater);
DECKBACK_TEST_DECL(cdm_fetcher);
DECKBACK_TEST_DECL(touchmode);
DECKBACK_TEST_DECL(platform);

int main(int argc, char** argv) {
  static const DeckbackTestCase kCases[] = {
      {"watchdog", watchdog_test_main},       {"updater", updater_test_main},
      {"cdm_fetcher", cdm_fetcher_test_main}, {"touchmode", touchmode_test_main},
      {"platform", platform_test_main},
  };
  return DECKBACK_RUN_GROUP(kCases);
}
