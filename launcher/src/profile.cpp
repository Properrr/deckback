#include "profile.hpp"

#include <filesystem>
#include <format>
#include <cstdlib>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "log.hpp"

namespace deckback {
namespace {

namespace fs = std::filesystem;

bool is_empty_directory(const fs::path& path) {
  std::error_code error;
  return fs::is_directory(fs::symlink_status(path, error)) &&
         fs::directory_iterator(path, error) == fs::directory_iterator{};
}

bool contains_symlink(const fs::path& root) {
  std::error_code error;
  const fs::file_status root_status = fs::symlink_status(root, error);
  if (error || fs::is_symlink(root_status)) return true;
  if (!fs::is_directory(root_status)) return false;

  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                            error),
       end;
       it != end && !error; it.increment(error)) {
    if (fs::is_symlink(it->symlink_status(error))) return true;
  }
  return false;
}

bool copy_profile(const fs::path& source, const fs::path& staging) {
  std::error_code error;
  fs::create_directories(staging, error);
  if (error) return false;

  for (const fs::directory_entry& entry : fs::directory_iterator(source, error)) {
    if (error) return false;
    const fs::path destination = staging / entry.path().filename();
    fs::copy(entry.path(), destination, fs::copy_options::recursive, error);
    if (error) return false;
  }
  return true;
}

}  // namespace

std::string migrate_legacy_profile(const std::string& durable_profile,
                                   const std::string& runtime_dir) {
  const fs::path target(durable_profile);
  std::error_code error;
  const bool target_exists = fs::exists(target, error);
  if (error) {
    warn(std::format("profile: cannot inspect durable path {}: {}", target.string(), error.message()));
    return {};
  }
  if (target_exists && !is_empty_directory(target)) {
    info("profile: durable profile already exists; legacy migration skipped");
    return {};
  }

  std::vector<fs::path> candidates;
  candidates.emplace_back(fs::path(runtime_dir) / "deckback-profile");
  if (const char* config_home = std::getenv("XDG_CONFIG_HOME"); config_home && *config_home)
    candidates.emplace_back(fs::path(config_home) / "content_shell");
  if (const char* home = std::getenv("HOME"); home && *home)
    candidates.emplace_back(fs::path(home) / ".config/content_shell");

  fs::path source;
  for (const fs::path& candidate : candidates) {
    if (candidate == target || !fs::is_directory(candidate, error) || error ||
        fs::is_empty(candidate, error) || error)
      continue;
    if (contains_symlink(candidate)) {
      warn(std::format("profile: refusing legacy profile with symlink: {}", candidate.string()));
      continue;
    }
    source = candidate;
    break;
  }
  if (source.empty()) return {};

  const fs::path parent = target.parent_path();
  const fs::path staging = parent / std::format(".profile-migration-{}", static_cast<long>(getpid()));
  fs::remove_all(staging, error);
  fs::create_directories(parent, error);
  if (error || !copy_profile(source, staging)) {
    fs::remove_all(staging, error);
    warn(std::format("profile: migration from {} failed; durable profile left untouched",
                     source.string()));
    return {};
  }

  if (target_exists) fs::remove_all(target, error);
  fs::rename(staging, target, error);
  if (error) {
    fs::remove_all(staging, error);
    warn(std::format("profile: cannot promote migration into {}", target.string()));
    return {};
  }
  info(std::format("profile: migrated legacy profile {} -> {}", source.string(), target.string()));
  return source.string();
}

}  // namespace deckback
