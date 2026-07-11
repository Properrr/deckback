#pragma once
#include <string>
#include <string_view>

#include "devtools.hpp"

namespace deckback {

// The kiosk failure state (findings input-ux §7; doc P2).
//
// When the network is down, `Page.navigate` fails and the engine renders Chromium's desktop error
// interstitial: a mouse-and-keyboard page with a "Reload" button nothing can focus, inside an app
// with no address bar, no keyboard and no way out. It is a hard dead-end — the user's only recourse
// is the Steam overlay. Roku's design principles put it plainly: a TV app owes the user an in-app,
// **controller-focusable** Retry, never a browser error page.
//
// This is also the surface that catches the project's biggest external risk (R1): if Leanback
// changes under us and stops loading, this page is what the user sees, and its text ships in
// `app.json` — so a hotfix is a config push, not a rebuild.

// ---- pure helpers (no CDP) — unit-tested in tests/errorpage_test.cpp
// -----------------------------

// What to show. Every string is user-visible and lands inside a JS string literal, so all of it is
// escaped on the way in.
struct ErrorPageInfo {
  std::string title = "Can't reach YouTube";
  std::string detail;  // the raw net:: error, shown small — for the person filing the bug report
  std::string hint;    // what the user can actually do about it
  std::string url;     // where we were trying to go
};

// Map a CDP `errorText` to a hint a person can act on. An unrecognised error yields a generic hint
// rather than a guess: telling someone to check their Wi-Fi when the certificate expired wastes
// their time. Returns "" for an empty error text.
std::string classify_net_error(std::string_view error_text);

// The JS that *replaces the current document* with the error page and installs its key handler.
// Pure, so the escaping is testable — and it must be, because a syntax error here is invisible: the
// page simply never appears and the user keeps staring at Chromium's interstitial.
//
// The page is injected rather than loaded from a bundled file: a `file://` load inside the Flatpak
// sandbox is one more thing to get wrong, and a `data:` top-frame navigation is restricted in
// Chromium. `about:blank` plus an injection needs neither.
//
// Note the retry signal goes launcher -> page -> launcher: the page sets `window.__deckbackRetry`
// and the launcher polls it. The launcher owns *when* to retry, because only it knows whether the
// network came back; the page only knows the user pressed a button.
std::string error_page_js(const ErrorPageInfo& info);

// Reads and clears the page's retry flag in one round trip. Reading without clearing would retry
// forever off one keypress.
extern const char* kRetryFlagExpr;

// True when the loaded document is our error page (rather than Leanback, or a blank engine). Used
// to decide whether an injected page is still up.
extern const char* kIsErrorPageExpr;

// Exponential backoff for automatic retries, doubling from `min_ms` and capped at `max_ms`.
// `attempt` is 0-based. Nonsense bounds are normalised rather than trusted: `app.json` is remotely
// hot-swappable, and a zero delay would spin the navigator against a dead network.
long retry_backoff_ms(int attempt, long min_ms, long max_ms);

// ---- CDP ---------------------------------------------------------------------------------------

// Navigate to about:blank and inject the error page. Returns false if the engine is unreachable —
// in which case there is nothing to draw on and the caller should just keep retrying.
bool show_error_page(DevToolsClient& client, const ErrorPageInfo& info);

// Did the user ask to retry? Consumes the flag.
bool take_retry_request(DevToolsClient& client);

}  // namespace deckback
