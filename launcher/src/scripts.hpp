#pragma once
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace deckback {

class DevToolsClient;

// ---- page-side scripts (findings durable/page-scripts.md) ---------------------------------------
//
// Every behaviour the launcher injects into youtube.com/tv is authored as a real .js file in
// config/scripts/ and reached through this one registry, instead of being hand-built as a C++
// string literal at each call site. Two problems that used to recur are solved centrally here:
//
//   * escaping — parameters are JSON-encoded through ONE escaper (ScriptParams), so no call site
//     concatenates JS by hand and gets the quoting subtly wrong;
//   * hot-swap — a script embedded at build time can be shadowed at runtime by a same-named .js in
//     the scripts dir, giving injected behaviours the same ship-a-fix-without-a-rebuild surface
//     app.json already has (doc §6, the R1 hotfix posture).
//
// Convention: each script file is a single PARENTHESISED FUNCTION EXPRESSION `(function(p){ ...
// })`. `render()` appends the params object, so the script receives its arguments as one object `p`
// and the call site never builds JS text. A no-arg script still takes `p` (it ignores it) and is
// invoked with `{}`. Keeping the file a valid standalone expression means it lints and can be
// exercised on its own.

// A JSON object literal builder for a script's `p` argument. Numbers/bools/strings/arrays are
// encoded once, here, so escaping is not re-invented per feature.
class ScriptParams {
 public:
  ScriptParams& set(std::string_view key, long value);
  ScriptParams& set(std::string_view key, int value) { return set(key, static_cast<long>(value)); }
  ScriptParams& set(std::string_view key, double value);
  ScriptParams& set(std::string_view key, bool value);
  ScriptParams& set(std::string_view key, std::string_view value);                 // JSON string
  ScriptParams& set(std::string_view key, const std::vector<std::string>& value);  // JSON string[]
  // JSON array of 2-element arrays, [["k1","v1"],...] — each string escaped ONCE. For structured
  // rows (e.g. the controls card's control/action pairs) that must not be pre-escaped by the
  // caller.
  ScriptParams& set(std::string_view key,
                    const std::vector<std::pair<std::string, std::string>>& value);
  ScriptParams& set_raw(std::string_view key, std::string_view js_expr);  // unescaped, verbatim

  // The serialized object, e.g. {"delta":-10,"text":"hi"}. "{}" when empty.
  std::string json() const;

 private:
  std::string body_;  // comma-joined "key":value pairs, no braces
};

// Process-wide registry of the page scripts. One instance (a singleton), initialised to the
// embedded defaults on first use and optionally pointed at a runtime override dir in main().
class ScriptLibrary {
 public:
  static ScriptLibrary& instance();  // thread-safe first-use init to the embedded defaults

  // Shadow embedded defaults with same-named .js files found in `dir` (each <name>.js overrides the
  // default of that name). Call ONCE in main() before any worker thread starts — after that the
  // library is read-only and safely shared across threads. An absent/empty dir is a no-op.
  void load_overrides(const std::string& dir);

  // The (possibly overridden) body of `name`, or "" if there is no such script.
  std::string_view body(std::string_view name) const;

  // The full expression to evaluate: `(body)(params)`. "" (logged once) if `name` is unknown.
  std::string render(std::string_view name, const ScriptParams& params = {}) const;

  // eval_void(render(name, params)). false if the script is unknown or the engine rejected it.
  bool invoke(DevToolsClient& client, std::string_view name, const ScriptParams& params = {}) const;

  // add_script_on_new_document(render(name)) — for scripts that must run on every document
  // (Page.addScriptToEvaluateOnNewDocument). false if unknown or the engine rejected it.
  bool install_sticky(DevToolsClient& client, std::string_view name) const;

 private:
  ScriptLibrary();
  std::map<std::string, std::string, std::less<>> scripts_;
};

}  // namespace deckback
