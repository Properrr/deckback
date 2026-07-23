#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"

namespace deckback {

struct Config;

// Sparse user-settings overlay: user.json, the launcher's first config WRITER (configurator-plan
// C2, osd-menu-plan O4/O10). Only user-changed, allowlisted keys live here. It loads AFTER app.json
// and overrides those fields, so shipped defaults (UA, cobalt_flags, the R1 hotfix keys) keep
// flowing through updates; a per-key reset drops the key, and "reset all" deletes the file. Writes
// are atomic (tmp + fsync + rename). Single-threaded: constructed and used on the launcher's input
// thread (the OSD's exec()), never concurrently.
class ConfigStore {
 public:
  explicit ConfigStore(std::string path);

  // Parse the overlay if the file exists. A missing file = no overrides; a corrupt file is logged
  // and treated as empty (it must never stop the app booting). Non-allowlisted keys are dropped.
  void load();

  // Overlay the retained keys onto an already-app.json-loaded Config.
  void apply(Config& c) const;

  // Set/clear one allowlisted key and persist. Non-allowlisted keys are refused (logged). The
  // in-memory value updates even if the disk write fails, so the live session still reflects it.
  bool set_string(std::string_view key, std::string_view value);
  bool set_bool(std::string_view key, bool value);
  bool set_string_list(std::string_view key, const std::vector<std::string>& value);
  bool remove(std::string_view key);  // per-key reset

  bool has(std::string_view key) const;
  const std::string& path() const { return path_; }

 private:
  bool put(std::string_view key, json::Value v);
  bool save() const;

  std::string path_;
  std::vector<json::Member> overlay_;  // key order preserved across rewrites
};

// Whether `key` may appear in user.json. Defense for R1/remote config: the user channel must not
// fight the shipped/remote channel over UA, cobalt_flags, url, etc. v1 = the caption settings.
bool is_overlayable(std::string_view key);

}  // namespace deckback
