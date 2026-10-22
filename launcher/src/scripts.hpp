#pragma once
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace deckback {

class DevToolsClient;

// ---- page-side scripts --------------------------------------------------------------------------
//
// Every script injected into youtube.com/tv is a .js file in config/scripts/, embedded at build
// time and optionally shadowed at runtime by a same-named file in the scripts dir. Parameters are
// JSON-encoded through ScriptParams, so no call site builds JS text by hand.
//
// Convention: a one-shot script is a single parenthesised function expression `(function(p){...})`;
// `render()` appends the params object (`{}` when there are none). Sticky scripts differ: see
// install_sticky().

// A JSON object literal builder for a script's `p` argument; every value is escaped here.
class ScriptParams {
 public:
  ScriptParams& set(std::string_view key, long value);
  ScriptParams& set(std::string_view key, int value) { return set(key, static_cast<long>(value)); }
  ScriptParams& set(std::string_view key, double value);
  ScriptParams& set(std::string_view key, bool value);
  ScriptParams& set(std::string_view key, std::string_view value);                 // JSON string
  ScriptParams& set(std::string_view key, const std::vector<std::string>& value);  // JSON string[]
  // JSON array of 2-element arrays, [["k1","v1"],...]; the caller must not pre-escape.
  ScriptParams& set(std::string_view key,
                    const std::vector<std::pair<std::string, std::string>>& value);
  ScriptParams& set_raw(std::string_view key, std::string_view js_expr);  // unescaped, verbatim

  // The serialized object, e.g. {"delta":-10,"text":"hi"}. "{}" when empty.
  std::string json() const;

 private:
  std::string body_;  // comma-joined "key":value pairs, no braces
};

// Process-wide registry of the page scripts, initialised to the embedded defaults on first use.
class ScriptLibrary {
 public:
  static ScriptLibrary& instance();  // thread-safe first-use init to the embedded defaults

  // Shadow embedded defaults with same-named <name>.js files in `dir`. Call ONCE in main() before
  // any worker thread starts; afterwards the library is read-only and thread-safe. An absent/empty
  // dir is a no-op.
  void load_overrides(const std::string& dir);

  // The (possibly overridden) body of `name`, or "" if there is no such script.
  std::string_view body(std::string_view name) const;

  // The full expression to evaluate: `(body)(params)`. "" (logged once) if `name` is unknown.
  std::string render(std::string_view name, const ScriptParams& params = {}) const;

  // eval_void(render(name, params)). false if the script is unknown or the engine rejected it.
  bool invoke(DevToolsClient& client, std::string_view name, const ScriptParams& params = {}) const;

  // add_script_on_new_document(body(name)), verbatim with no params: sticky scripts are
  // self-invoking `(function(){...})();`. false if unknown or the engine rejected it.
  bool install_sticky(DevToolsClient& client, std::string_view name) const;

 private:
  ScriptLibrary();
  std::map<std::string, std::string, std::less<>> scripts_;
};

}  // namespace deckback
