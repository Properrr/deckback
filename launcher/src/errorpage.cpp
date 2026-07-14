#include "errorpage.hpp"

#include "log.hpp"
#include "scripts.hpp"

namespace deckback {

const char* kRetryFlagExpr =
    "(function(){var r=!!window.__deckbackRetry;window.__deckbackRetry=false;return r;})()";

const char* kIsErrorPageExpr = "!!document.getElementById('__deckback_error')";

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
  // The page markup, style, and Trusted Types policy now live in config/scripts/error_page.js
  // (ScriptLibrary); title/hint/url/detail are JSON string params, escaped once by ScriptParams —
  // this call site no longer hand-escapes JS. Enter/Space retry; Escape is deliberately unbound.
  // The script's policy falls back to the raw string on about:blank, which has no Trusted Types.
  return ScriptLibrary::instance().render("error_page", ScriptParams()
                                                            .set("title", info.title)
                                                            .set("hint", info.hint)
                                                            .set("url", info.url)
                                                            .set("detail", info.detail));
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
