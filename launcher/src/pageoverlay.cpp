#include "pageoverlay.hpp"

namespace deckback {

bool PageOverlay::draw(std::string_view script_name, const ScriptParams& params) {
  const bool ok = ScriptLibrary::instance().invoke(client_, script_name, params);
  if (ok) painted_.store(true, std::memory_order_relaxed);
  return ok;
}

bool PageOverlay::draw_js(std::string_view js) {
  if (js.empty()) return false;  // an unknown script renders "" — never call that a paint
  const bool ok = client_.eval_void(js);
  if (ok) painted_.store(true, std::memory_order_relaxed);
  return ok;
}

void PageOverlay::hide(std::string_view hide_script_name) {
  if (!set_painted(false)) return;  // already hidden: do not spend a round trip per button press
  ScriptLibrary::instance().invoke(client_, hide_script_name);
}

bool PageOverlay::lost() {
  if (!painted()) return false;
  if (!probe_.due()) return false;
  // The comment marker keeps this expression distinguishable in a CDP trace; it costs nothing.
  const std::optional<bool> present =
      client_.eval_bool("/*overlay-state*/!!document.getElementById('" + element_id_ + "')");
  if (!present || *present) return false;  // still there, or the engine did not answer
  painted_.store(false, std::memory_order_relaxed);
  return true;
}

}  // namespace deckback
