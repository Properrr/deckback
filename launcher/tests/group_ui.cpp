// The on-screen surfaces and the input layer that drives them.
//
// One binary for this group instead of one per file (see harness.hpp). CTest still registers each
// case by name and runs it in its own process, so both the reporting and the isolation are
// unchanged — only the link steps are fewer.
#include "harness.hpp"

DECKBACK_TEST_DECL(onboarding);
DECKBACK_TEST_DECL(osdmenu);
DECKBACK_TEST_DECL(overlay);
DECKBACK_TEST_DECL(updateprompt);
DECKBACK_TEST_DECL(input);

int main(int argc, char** argv) {
  static const DeckbackTestCase kCases[] = {
      {"onboarding", onboarding_test_main}, {"osdmenu", osdmenu_test_main},
      {"overlay", overlay_test_main},       {"updateprompt", updateprompt_test_main},
      {"input", input_test_main},
  };
  return DECKBACK_RUN_GROUP(kCases);
}
