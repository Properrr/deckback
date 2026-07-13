#include "errorpage.hpp"

#include <format>

#include "log.hpp"
#include "overlay.hpp"  // js_trusted_html
#include "util.hpp"     // js_string_escape

namespace deckback {

const char* kRetryFlagExpr =
    "(function(){var r=!!window.__deckbackRetry;window.__deckbackRetry=false;return r;})()";

const char* kIsErrorPageExpr = "!!document.getElementById('__deckback_error')";

namespace {

// Sized for a 1280x720 buffer letterboxed onto the 800p panel, viewed at arm's length on a 7"
// screen — the same reasoning as the toast. The button carries a visible focus ring because on a
// controller there is no cursor to tell you what "Enter" will hit.
constexpr const char* kErrorPageStyle =
    "<style>"
    "html,body{margin:0;height:100%;background:#0f0f0f;color:#fff;"
    "font:400 24px/1.45 system-ui,Roboto,Arial,sans-serif;}"
    "#__deckback_error{box-sizing:border-box;height:100%;display:flex;flex-direction:column;"
    "align-items:center;justify-content:center;text-align:center;padding:6vh 8vw;gap:14px;}"
    "h1{margin:0;font-size:44px;font-weight:600;letter-spacing:-.01em;}"
    "p{margin:0;max-width:26em;color:#c9c9c9;}"
    "#__deckback_retry{margin-top:22px;font:600 26px/1 system-ui,sans-serif;color:#0f0f0f;"
    "background:#fff;border:0;border-radius:999px;padding:18px 44px;cursor:pointer;}"
    "#__deckback_retry:focus{outline:4px solid #3ea6ff;outline-offset:4px;}"
    "small{color:#7a7a7a;font-size:15px;word-break:break-all;max-width:34em;}"
    "</style>";

}  // namespace

std::string classify_net_error(std::string_view error_text) {
  if (error_text.empty()) return {};
  auto has = [&](std::string_view n) { return error_text.find(n) != std::string_view::npos; };

  // Only errors we can say something *useful* about. Everything else gets the generic hint: a wrong
  // instruction ("check your Wi-Fi" for an expired certificate) is worse than an honest vague one,
  // because the user will follow it.
  if (has("ERR_INTERNET_DISCONNECTED") || has("ERR_NETWORK_CHANGED") ||
      has("ERR_ADDRESS_UNREACHABLE"))
    return "This Steam Deck isn't connected to a network.";
  if (has("ERR_NAME_NOT_RESOLVED") || has("ERR_NAME_RESOLUTION_FAILED"))
    return "Connected, but the network can't look up youtube.com. Check your Wi-Fi or DNS.";
  if (has("ERR_PROXY_CONNECTION_FAILED") || has("ERR_TUNNEL_CONNECTION_FAILED"))
    return "A proxy or VPN on this network is refusing the connection.";
  if (has("ERR_CONNECTION_TIMED_OUT") || has("ERR_TIMED_OUT"))
    return "The connection timed out. The network may be captive (a hotel or campus sign-in page).";
  if (has("ERR_CERT_") || has("ERR_SSL_"))
    return "The secure connection couldn't be verified. Check the Deck's clock and date.";
  if (has("ERR_CONNECTION_REFUSED") || has("ERR_CONNECTION_RESET") ||
      has("ERR_CONNECTION_CLOSED") || has("ERR_EMPTY_RESPONSE"))
    return "YouTube refused the connection. It may be blocked on this network.";
  return "Deckback will keep trying while you sort it out.";
}

long retry_backoff_ms(int attempt, long min_ms, long max_ms) {
  if (min_ms < 250) min_ms = 250;  // never spin the navigator against a dead network
  // Two clamps that look necessary and are not, so they are absent rather than decorative:
  //   * `max_ms < min_ms` — `delay` starts at min_ms and the loop needs `delay < max_ms`, so an
  //     inverted range simply never doubles and returns min_ms, which is what a clamp would give.
  //   * `attempt < 0` — the loop never runs, and returns min_ms.
  long delay = min_ms;
  for (int i = 0; i < attempt && delay < max_ms; ++i) {
    // Cap *before* doubling, not after. `delay *= 2` on a delay past max_ms/2 can overflow when
    // max_ms is itself near LONG_MAX — signed overflow is UB, and the plausible outcome is a
    // negative delay, i.e. an immediate retry: a busy loop that also re-navigates every iteration.
    // This also makes a post-loop cap unnecessary, since `delay` can never exceed max_ms here.
    if (delay > max_ms / 2) return max_ms;
    delay *= 2;
  }
  return delay;
}

std::string error_page_js(const ErrorPageInfo& info) {
  // documentElement.innerHTML, not document.write: we own about:blank here, and a <script> inserted
  // via innerHTML would never execute — so the key handler is attached from this same evaluate.
  //
  // Enter and Space retry; nothing else does. Escape is deliberately NOT bound: the only thing it
  // could do is quit, and quitting the app because the Wi-Fi blinked is not a kindness.
  // about:blank has no Trusted Types, so js_trusted_html returns the raw string here and the page
  // is unchanged. It is wrapped anyway so this never becomes the next controls-card surprise if we
  // ever render the error surface over a page that does carry the CSP.
  const std::string html = std::format(
      "\"{}<body><div id='__deckback_error'>"
      "<h1>{}</h1><p>{}</p>"
      "<button id='__deckback_retry'>Try again</button>"
      "<small>{}</small><small>{}</small>"
      "</div></body>\"",
      js_string_escape(kErrorPageStyle), js_string_escape(info.title), js_string_escape(info.hint),
      js_string_escape(info.url), js_string_escape(info.detail));
  return std::format(
      "(function(){{"
      "document.documentElement.innerHTML={};"
      "window.__deckbackRetry=false;"
      "var b=document.getElementById('__deckback_retry');"
      "if(b){{b.focus();b.addEventListener('click',function(){{window.__deckbackRetry=true;}});}}"
      "document.addEventListener('keydown',function(e){{"
      "if(e.key==='Enter'||e.key===' '||e.key==='Spacebar')window.__deckbackRetry=true;}});"
      "return true;}})()",
      js_trusted_html(html));
}

bool show_error_page(DevToolsClient& client, const ErrorPageInfo& info) {
  // about:blank always commits, so unlike a bundled file:// page this cannot itself fail with the
  // very error we are trying to report.
  if (!client.navigate("about:blank")) return false;
  return client.eval_void(error_page_js(info));
}

bool take_retry_request(DevToolsClient& client) {
  auto r = client.eval_bool(kRetryFlagExpr);
  return r.value_or(false);
}

}  // namespace deckback
