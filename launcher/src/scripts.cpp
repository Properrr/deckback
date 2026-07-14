#include "scripts.hpp"

#include <format>
#include <fstream>
#include <sstream>

#include "devtools.hpp"
#include "log.hpp"
#include "scripts_registry.hpp"  // GENERATED: kEmbeddedScripts[] {name, body}
#include "util.hpp"              // js_string_escape

namespace deckback {

// ---- ScriptParams -------------------------------------------------------------------------------

namespace {
void append_key(std::string& body, std::string_view key) {
  if (!body.empty()) body += ',';
  body += '"';
  body += js_string_escape(key);
  body += "\":";
}
}  // namespace

ScriptParams& ScriptParams::set(std::string_view key, long value) {
  append_key(body_, key);
  body_ += std::format("{}", value);
  return *this;
}

ScriptParams& ScriptParams::set(std::string_view key, double value) {
  append_key(body_, key);
  body_ += std::format("{}", value);
  return *this;
}

ScriptParams& ScriptParams::set(std::string_view key, bool value) {
  append_key(body_, key);
  body_ += value ? "true" : "false";
  return *this;
}

ScriptParams& ScriptParams::set(std::string_view key, std::string_view value) {
  append_key(body_, key);
  body_ += '"';
  body_ += js_string_escape(value);
  body_ += '"';
  return *this;
}

ScriptParams& ScriptParams::set(std::string_view key, const std::vector<std::string>& value) {
  append_key(body_, key);
  body_ += '[';
  bool first = true;
  for (const std::string& s : value) {
    if (!first) body_ += ',';
    first = false;
    body_ += '"';
    body_ += js_string_escape(s);
    body_ += '"';
  }
  body_ += ']';
  return *this;
}

ScriptParams& ScriptParams::set(std::string_view key,
                                const std::vector<std::pair<std::string, std::string>>& value) {
  append_key(body_, key);
  body_ += '[';
  bool first = true;
  for (const auto& [a, b] : value) {
    if (!first) body_ += ',';
    first = false;
    body_ += "[\"";
    body_ += js_string_escape(a);
    body_ += "\",\"";
    body_ += js_string_escape(b);
    body_ += "\"]";
  }
  body_ += ']';
  return *this;
}

ScriptParams& ScriptParams::set_raw(std::string_view key, std::string_view js_expr) {
  append_key(body_, key);
  body_ += js_expr;
  return *this;
}

std::string ScriptParams::json() const { return "{" + body_ + "}"; }

// ---- ScriptLibrary ------------------------------------------------------------------------------

ScriptLibrary::ScriptLibrary() {
  for (const EmbeddedScript& s : kEmbeddedScripts) scripts_.emplace(s.name, s.body);
}

ScriptLibrary& ScriptLibrary::instance() {
  // Meyers singleton: thread-safe initialisation on first use. load_overrides() is expected before
  // any worker thread starts, so the map is read-only by the time it is shared.
  static ScriptLibrary lib;
  return lib;
}

void ScriptLibrary::load_overrides(const std::string& dir) {
  if (dir.empty()) return;
  int n = 0;
  for (auto& [name, body] : scripts_) {
    std::ifstream f(dir + "/" + name + ".js", std::ios::binary);
    if (!f) continue;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string override_body = ss.str();
    // A present-but-empty file is a mistake (an editor truncation, a failed write); keeping the
    // embedded default is safer than injecting nothing and silently disabling the behaviour.
    if (override_body.find_first_not_of(" \t\r\n") == std::string::npos) continue;
    body = std::move(override_body);
    ++n;
    info(std::format("scripts: '{}' overridden from {}", name, dir));
  }
  if (n == 0)
    info(std::format("scripts: no runtime overrides in {} (embedded defaults in use)", dir));
}

std::string_view ScriptLibrary::body(std::string_view name) const {
  auto it = scripts_.find(name);
  return it == scripts_.end() ? std::string_view{} : std::string_view(it->second);
}

std::string ScriptLibrary::render(std::string_view name, const ScriptParams& params) const {
  auto it = scripts_.find(name);
  if (it == scripts_.end()) {
    warn(std::format("scripts: no script named '{}' (call-site typo?)", name));
    return {};
  }
  return it->second + "(" + params.json() + ")";
}

bool ScriptLibrary::invoke(DevToolsClient& client, std::string_view name,
                           const ScriptParams& params) const {
  std::string js = render(name, params);
  return js.empty() ? false : client.eval_void(js);
}

bool ScriptLibrary::install_sticky(DevToolsClient& client, std::string_view name) const {
  // Sticky scripts are installed VERBATIM (no params appended): they are self-invoking
  // document-start scripts `(function(){...})();`, not the `(function(p){...})` one-shot shape
  // render() feeds. `just smoke` injects the same file verbatim, so the two paths run identical
  // text.
  std::string_view b = body(name);
  if (b.empty()) {
    warn(std::format("scripts: no sticky script named '{}' (call-site typo?)", name));
    return false;
  }
  return client.add_script_on_new_document(b);
}

}  // namespace deckback
