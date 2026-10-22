#pragma once
#include <string>
#include <string_view>

#include "devtools.hpp"

namespace deckback {

// The kiosk failure state: a controller-focusable Retry page that replaces Chromium's error
// interstitial when a navigation fails. Its text can be overridden from `app.json`.

// ---- pure helpers (no CDP) ----

// What to show. Every string is user-visible and is escaped into a JS string literal.
struct ErrorPageInfo {
  std::string title = "Can't reach YouTube";
  std::string detail;  // the raw net:: error, shown small
  std::string hint;    // what the user can actually do about it
  std::string url;     // where we were trying to go
};

// Map a CDP `errorText` to an actionable hint; an unrecognised error yields a generic hint rather
// than a guess. Returns "" for an empty error text.
std::string classify_net_error(std::string_view error_text);

// The JS that replaces the current document with the error page and installs its key handler. A
// syntax error here fails invisibly. The page sets `window.__deckbackRetry` on a retry press and
// the launcher polls it (kRetryFlagExpr); the launcher owns when to retry.
std::string error_page_js(const ErrorPageInfo& info);

// Reads and clears the page's retry flag in one round trip.
extern const char* kRetryFlagExpr;

// True when the loaded document is our error page (rather than Leanback, or a blank engine).
extern const char* kIsErrorPageExpr;

// Exponential backoff for automatic retries, doubling from `min_ms` and capped at `max_ms`.
// `attempt` is 0-based. Bounds come from remotely-overridable config, so they are normalised.
long retry_backoff_ms(int attempt, long min_ms, long max_ms);

// ---- CDP ---------------------------------------------------------------------------------------

// Navigate to about:blank and inject the error page. Returns false if the engine is unreachable.
bool show_error_page(DevToolsClient& client, const ErrorPageInfo& info);

// Did the user ask to retry? Consumes the flag.
bool take_retry_request(DevToolsClient& client);

}  // namespace deckback
