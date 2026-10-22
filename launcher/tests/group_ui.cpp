// The on-screen surfaces and the input layer that drives them.
// Linked as one binary; CTest runs each case in its own process (see harness.hpp).
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
