#pragma once

#include <string>

namespace deckback {

// Migrate a legacy profile into an empty durable profile. Returns the source path when a migration
// succeeds, or an empty string when no migration is needed/possible.
std::string migrate_legacy_profile(const std::string& durable_profile,
                                   const std::string& runtime_dir);

}  // namespace deckback
