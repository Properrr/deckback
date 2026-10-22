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

  // Only errors with a useful, correct instruction; everything else gets the generic hint.
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
  // An inverted range or a negative `attempt` never enters the loop and returns min_ms.
  long delay = min_ms;
  for (int i = 0; i < attempt && delay < max_ms; ++i) {
    // Cap before doubling so `delay *= 2` cannot overflow when max_ms is near LONG_MAX.
    if (delay > max_ms / 2) return max_ms;
    delay *= 2;
  }
  return delay;
}

std::string error_page_js(const ErrorPageInfo& info) {
  // config/scripts/error_page.js carries its own Trusted Types policy, which falls back to the raw
  // string on about:blank. Enter/Space retry; Escape is deliberately unbound.
  return ScriptLibrary::instance().render("error_page", ScriptParams()
                                                            .set("title", info.title)
                                                            .set("hint", info.hint)
                                                            .set("url", info.url)
                                                            .set("detail", info.detail));
}

bool show_error_page(DevToolsClient& client, const ErrorPageInfo& info) {
  // about:blank always commits, so it cannot fail with the error being reported.
  if (!client.navigate("about:blank")) return false;
  return client.eval_void(error_page_js(info));
}

bool take_retry_request(DevToolsClient& client) {
  auto r = client.eval_bool(kRetryFlagExpr);
  return r.value_or(false);
}

}  // namespace deckback
