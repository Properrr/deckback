#include "profile.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

int main() {
  const fs::path root = fs::temp_directory_path() / "deckback-profile-test";
  fs::remove_all(root);
  fs::create_directories(root / "runtime" / "deckback-profile/Local Storage");
  std::ofstream(root / "runtime/deckback-profile/Cookies") << "legacy";

  setenv("HOME", (root / "home").c_str(), 1);
  const fs::path target = root / "home/.local/share/deckback/profile";
  const std::string migrated = deckback::migrate_legacy_profile(
      target.string(), (root / "runtime").string());
  assert(migrated == (root / "runtime/deckback-profile").string());
  assert(fs::exists(target / "Cookies"));

  std::ofstream(target / "Preferences") << "durable";
  fs::remove_all(root / "runtime/deckback-profile");
  fs::create_directories(root / "runtime/deckback-profile");
  std::ofstream(root / "runtime/deckback-profile/Cookies") << "legacy-2";
  assert(deckback::migrate_legacy_profile(target.string(), (root / "runtime").string()).empty());
  std::ifstream durable(target / "Preferences");
  std::string contents;
  durable >> contents;
  assert(contents == "durable");

  fs::remove_all(root);
}
