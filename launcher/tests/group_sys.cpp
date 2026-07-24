// Process, platform and network-facing subsystems.
//
// One binary for this group instead of one per file (see harness.hpp). CTest still registers each
// case by name and runs it in its own process, so both the reporting and the isolation are
// unchanged — only the link steps are fewer.
#include "harness.hpp"

DECKBACK_TEST_DECL(watchdog);
DECKBACK_TEST_DECL(updater);
DECKBACK_TEST_DECL(cdm_fetcher);
DECKBACK_TEST_DECL(touchmode);

int main(int argc, char** argv) {
  static const DeckbackTestCase kCases[] = {
      {"watchdog", watchdog_test_main},
      {"updater", updater_test_main},
      {"cdm_fetcher", cdm_fetcher_test_main},
      {"touchmode", touchmode_test_main},
  };
  return DECKBACK_RUN_GROUP(kCases);
}
