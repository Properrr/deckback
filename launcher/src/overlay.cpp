#include "overlay.hpp"

#include "scripts.hpp"

namespace deckback {

std::string toast_js(std::string_view text, int ms) {
  return ScriptLibrary::instance().render("toast", ScriptParams().set("text", text).set("ms", ms));
}

std::string toast_hide_js() { return ScriptLibrary::instance().render("toast_hide"); }

void show_toast(DevToolsClient& client, std::string_view text, int ms) {
  // Result ignored: the feedback is secondary to the action it reports.
  client.eval_void(toast_js(text, ms));
}

void hide_toast(DevToolsClient& client) { client.eval_void(toast_hide_js()); }

}  // namespace deckback
