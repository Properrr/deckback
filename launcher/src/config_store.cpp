#include "config_store.hpp"

#include <variant>

#include "config.hpp"
#include "fileio.hpp"
#include "log.hpp"

namespace deckback {
namespace {

// The keys user.json may override. One table drives both the allowlist (load/put) and apply(). The
// touch keys are here so an installed Flatpak (whose app.json is read-only) can toggle them.
using OverlayMember =
    std::variant<bool Config::*, std::string Config::*, std::vector<std::string> Config::*>;
struct OverlayKey {
  std::string_view key;
  OverlayMember member;
};
const OverlayKey kOverlayable[] = {
    {"captions_toast", &Config::captions_toast},
    {"caption_remember", &Config::caption_remember},
    {"caption_type", &Config::caption_type},
    {"caption_language", &Config::caption_language},
    {"caption_languages", &Config::caption_languages},
    {"caption_control", &Config::caption_control},
    {"caption_on", &Config::caption_on},
    {"disable_touch", &Config::disable_touch},
    {"touch_gestures", &Config::touch_gestures},
    {"touch_hide_cursor", &Config::touch_hide_cursor},
};

// A value of the wrong JSON type leaves the field at its app.json value.
void assign(Config& c, bool Config::* m, const json::Value& v) {
  if (auto b = v.as_bool()) c.*m = *b;
}
void assign(Config& c, std::string Config::* m, const json::Value& v) {
  if (const std::string* s = v.as_string()) c.*m = *s;
}
void assign(Config& c, std::vector<std::string> Config::* m, const json::Value& v) {
  const std::vector<json::Value>* a = v.as_array();
  if (!a) return;
  std::vector<std::string> out;
  for (const json::Value& e : *a)
    if (const std::string* s = e.as_string()) out.push_back(*s);
  c.*m = std::move(out);
}

const json::Value* find_member(const std::vector<json::Member>& members, std::string_view key) {
  for (const json::Member& kv : members)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

}  // namespace

bool is_overlayable(std::string_view key) {
  for (const OverlayKey& k : kOverlayable)
    if (k.key == key) return true;
  return false;
}

ConfigStore::ConfigStore(std::string path) : path_(std::move(path)) {}

void ConfigStore::load() {
  overlay_.clear();
  const auto text = read_file(path_);
  if (!text) return;
  json::ParseResult pr = json::parse(*text);
  if (!pr.ok()) {
    warn("config: " + path_ + ":" + std::to_string(pr.error.line) + ": " + pr.error.message +
         " — ignoring user overrides");
    return;
  }
  const std::vector<json::Member>* obj = pr.value->as_object();
  if (!obj) {
    warn("config: " + path_ + " is not a JSON object — ignoring user overrides");
    return;
  }
  for (const json::Member& kv : *obj) {
    if (!is_overlayable(kv.first)) {
      warn("config: user.json key '" + kv.first + "' is not user-overridable — ignored");
      continue;
    }
    overlay_.push_back(kv);
  }
}

void ConfigStore::apply(Config& c) const {
  for (const OverlayKey& k : kOverlayable)
    if (const json::Value* v = find_member(overlay_, k.key))
      std::visit([&](auto m) { assign(c, m, *v); }, k.member);
}

bool ConfigStore::has(std::string_view key) const { return find_member(overlay_, key) != nullptr; }

bool ConfigStore::put(std::string_view key, json::Value v) {
  if (!is_overlayable(key)) {
    warn("config: refusing to persist non-overridable key '" + std::string(key) + "'");
    return false;
  }
  bool replaced = false;
  for (json::Member& m : overlay_)
    if (m.first == key) {
      m.second = std::move(v);
      replaced = true;
      break;
    }
  if (!replaced) overlay_.emplace_back(std::string(key), std::move(v));
  return save();
}

bool ConfigStore::set_string(std::string_view key, std::string_view value) {
  return put(key, json::Value(std::string(value)));
}

bool ConfigStore::set_bool(std::string_view key, bool value) {
  return put(key, json::Value(value));
}

bool ConfigStore::set_string_list(std::string_view key, const std::vector<std::string>& value) {
  std::vector<json::Value> arr;
  arr.reserve(value.size());
  for (const std::string& s : value) arr.push_back(json::Value(s));
  return put(key, json::Value::array(std::move(arr)));
}

bool ConfigStore::remove(std::string_view key) {
  for (auto it = overlay_.begin(); it != overlay_.end(); ++it)
    if (it->first == key) {
      overlay_.erase(it);
      return save();
    }
  return true;
}

bool ConfigStore::save() const {
  // Atomic: a crash mid-save leaves the previous user.json, never a truncated one.
  if (write_file_atomic(path_, json::dump(json::Value::object(overlay_)))) return true;
  warn("config: could not save " + path_);
  return false;
}

}  // namespace deckback
