#include "overlay.hpp"

#include <format>

namespace deckback {

std::string js_string_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

std::string js_trusted_html(std::string_view raw_expr) {
  // Memoise the policy on window; create it lazily and defensively. `createHTML` is identity: we are
  // not sanitising, only satisfying the type gate for HTML we generated ourselves. The whole thing
  // is one expression so it can drop straight into `d.innerHTML=<here>`.
  return std::format(
      "(function(_h){{try{{var T=window.trustedTypes;if(!T)return _h;"
      "if(!window.__dbTTP)window.__dbTTP=T.createPolicy('deckback',"
      "{{createHTML:function(x){{return x;}}}});"
      "return window.__dbTTP.createHTML(_h);}}catch(e){{return _h;}}}})({})",
      raw_expr);
}

namespace {

// Rendered from a 1280x720 buffer letterboxed onto the 800p panel, so the type is deliberately
// large: the Deck-Verified bar is a 9 px minimum glyph at 1280x800 and secondary metadata already
// sits near it. `pointer-events:none` keeps the toast from ever eating a tap.
constexpr const char* kToastStyle =
    "position:fixed;left:50%;top:7%;transform:translateX(-50%);z-index:2147483647;"
    "background:rgba(0,0,0,0.86);color:#fff;font:600 30px/1.25 system-ui,sans-serif;"
    "padding:18px 30px;border-radius:14px;pointer-events:none;white-space:pre;"
    "opacity:1;transition:opacity .35s ease-out;";

}  // namespace

std::string toast_js(std::string_view text, int ms) {
  if (ms < 0) ms = 0;
  // documentElement, not body: Leanback swaps body content on navigation, and an <html>-level child
  // survives longer. The node is reused by id so repeated toasts do not stack up.
  return std::format(
      "(function(){{var id='__deckback_toast';"
      "var d=document.getElementById(id);"
      "if(!d){{d=document.createElement('div');d.id=id;"
      "document.documentElement.appendChild(d);}}"
      "d.textContent=\"{}\";"
      "d.setAttribute('style',\"{}\");"
      "if(window.__deckbackToastT)clearTimeout(window.__deckbackToastT);"
      "window.__deckbackToastT=setTimeout(function(){{"
      "var n=document.getElementById(id);if(n)n.style.opacity='0';}},{});"
      "return true;}})()",
      js_string_escape(text), kToastStyle, ms);
}

std::string toast_hide_js() {
  return "(function(){var n=document.getElementById('__deckback_toast');"
         "if(window.__deckbackToastT)clearTimeout(window.__deckbackToastT);"
         "if(n)n.style.opacity='0';return true;})()";
}

void show_toast(DevToolsClient& client, std::string_view text, int ms) {
  // Deliberately ignores the result. A toast that fails to render must not stop the touch lock from
  // engaging — the feedback is secondary to the action it reports.
  client.eval_void(toast_js(text, ms));
}

void hide_toast(DevToolsClient& client) { client.eval_void(toast_hide_js()); }

}  // namespace deckback
