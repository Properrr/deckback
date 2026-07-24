// Pure logic and small file I/O: no sockets, no threads, no engine.
//
// One binary for this group instead of one per file (see harness.hpp). CTest still registers each
// case by name and runs it in its own process, so both the reporting and the isolation are
// unchanged — only the link steps are fewer.
#include "harness.hpp"

DECKBACK_TEST_DECL(json);
DECKBACK_TEST_DECL(util);
DECKBACK_TEST_DECL(sha256);
DECKBACK_TEST_DECL(config);
DECKBACK_TEST_DECL(config_store);
DECKBACK_TEST_DECL(caption_settings);
DECKBACK_TEST_DECL(caption_fuzz);
DECKBACK_TEST_DECL(about);
DECKBACK_TEST_DECL(scripts);
DECKBACK_TEST_DECL(log);
DECKBACK_TEST_DECL(profile);

int main(int argc, char** argv) {
  static const DeckbackTestCase kCases[] = {
      {"json", json_test_main},
      {"util", util_test_main},
      {"sha256", sha256_test_main},
      {"config", config_test_main},
      {"config_store", config_store_test_main},
      {"caption_settings", caption_settings_test_main},
      {"caption_fuzz", caption_fuzz_test_main},
      {"about", about_test_main},
      {"scripts", scripts_test_main},
      {"log", log_test_main},
      {"profile", profile_test_main},
  };
  return DECKBACK_RUN_GROUP(kCases);
}
