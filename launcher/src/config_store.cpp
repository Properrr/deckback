#include "config_store.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "config.hpp"
#include "log.hpp"

namespace deckback {
namespace {
namespace fs = std::filesystem;

constexpr std::string_view kOverlayable[] = {
    "captions_toast",    "caption_remember", "caption_type", "caption_language",
    "caption_languages", "caption_control",  "caption_on",
};

const json::Value* find_member(const std::vector<json::Member>& members, std::string_view key) {
  for (const json::Member& kv : members)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

}  // namespace

bool is_overlayable(std::string_view key) {
  for (std::string_view k : kOverlayable)
    if (k == key) return true;
  return false;
}

ConfigStore::ConfigStore(std::string path) : path_(std::move(path)) {}

void ConfigStore::load() {
  overlay_.clear();
  std::ifstream f(path_, std::ios::binary);
  if (!f) return;
  std::ostringstream ss;
  ss << f.rdbuf();
  json::ParseResult pr = json::parse(ss.str());
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
  if (const json::Value* v = find_member(overlay_, "captions_toast"))
    if (auto b = v->as_bool()) c.captions_toast = *b;
  if (const json::Value* v = find_member(overlay_, "caption_remember"))
    if (auto b = v->as_bool()) c.caption_remember = *b;
  if (const json::Value* v = find_member(overlay_, "caption_type"))
    if (auto s = v->as_string()) c.caption_type = *s;
  if (const json::Value* v = find_member(overlay_, "caption_control"))
    if (auto s = v->as_string()) c.caption_control = *s;
  if (const json::Value* v = find_member(overlay_, "caption_on"))
    if (auto b = v->as_bool()) c.caption_on = *b;
  if (const json::Value* v = find_member(overlay_, "caption_language"))
    if (auto s = v->as_string()) c.caption_language = *s;
  if (const json::Value* v = find_member(overlay_, "caption_languages"))
    if (const std::vector<json::Value>* a = v->as_array()) {
      std::vector<std::string> langs;
      for (const json::Value& e : *a)
        if (const std::string* s = e.as_string()) langs.push_back(*s);
      c.caption_languages = std::move(langs);
    }
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
  fs::path p(path_);
  std::error_code ec;
  if (p.has_parent_path()) {
    fs::create_directories(p.parent_path(), ec);
    if (ec) {
      warn("config: cannot create " + p.parent_path().string());
      return false;
    }
  }

  const std::string text = json::dump(json::Value::object(overlay_));
  const std::string tmp = path_ + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    warn("config: cannot open " + tmp + " for write");
    return false;
  }
  const char* data = text.data();
  size_t left = text.size();
  while (left > 0) {
    const ssize_t n = ::write(fd, data, left);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      ::close(fd);
      ::unlink(tmp.c_str());
      warn("config: write failed on " + tmp);
      return false;
    }
    data += n;
    left -= static_cast<size_t>(n);
  }
  const bool synced = ::fsync(fd) == 0;
  const bool closed = ::close(fd) == 0;
  if (!synced || !closed) {
    ::unlink(tmp.c_str());
    warn("config: sync failed on " + tmp);
    return false;
  }
  fs::rename(tmp, p, ec);
  if (ec) {
    fs::remove(tmp, ec);
    warn("config: rename failed for " + path_);
    return false;
  }
  return true;
}

}  // namespace deckback
