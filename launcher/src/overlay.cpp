#include "overlay.hpp"

#include "scripts.hpp"

namespace deckback {

std::string toast_js(std::string_view text, int ms) {
  // The toast markup + style now live in config/scripts/toast.js (ScriptLibrary), reached by name;
  // text/ms are passed as JSON params so this call site no longer hand-escapes JS.
  return ScriptLibrary::instance().render("toast", ScriptParams().set("text", text).set("ms", ms));
}

std::string toast_hide_js() { return ScriptLibrary::instance().render("toast_hide"); }

void show_toast(DevToolsClient& client, std::string_view text, int ms) {
  // Deliberately ignores the result. A toast that fails to render must not stop the touch lock from
  // engaging — the feedback is secondary to the action it reports.
  client.eval_void(toast_js(text, ms));
}

void hide_toast(DevToolsClient& client) { client.eval_void(toast_hide_js()); }

}  // namespace deckback
