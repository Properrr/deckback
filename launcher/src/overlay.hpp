#pragma once
#include <string>
#include <string_view>

#include "devtools.hpp"
#include "util.hpp"

namespace deckback {

// A CDP-injected DOM toast: a <div> appended to `document.documentElement`. It uses `textContent`,
// since youtube.com/tv's CSP requires a Trusted Types policy for any `.innerHTML` assignment.

// Build the JS for one toast (config/scripts/toast.js). The node is reused across calls (one id),
// auto-fades after `ms`, and is `pointer-events:none` so it never swallows a click.
std::string toast_js(std::string_view text, int ms);

// JS that removes the toast immediately (used when a state ends earlier than the timeout).
std::string toast_hide_js();

// Fire-and-forget: a failed toast must never break the thing it was announcing.
void show_toast(DevToolsClient& client, std::string_view text, int ms);
void hide_toast(DevToolsClient& client);

}  // namespace deckback
