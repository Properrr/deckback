#pragma once
#include <string>
#include <string_view>

#include "devtools.hpp"
#include "util.hpp"

namespace deckback {

// A CDP-injected DOM toast. The engine has no overlay surface of its own and we refuse to patch one
// in, so on-screen feedback is a <div> we append to `document.documentElement` over CDP. This is
// the same technique `navigator.cpp` already uses for codec steering — no engine patch, and it
// survives a Leanback navigation because we re-inject rather than persist.
//
// It exists because the touch lock is otherwise **unobservable**: findings input-ux §4 requires a
// glyph/toast on engage, and today a user who trips the chord sees a screen that has simply stopped
// responding to touch. That is the bug report this exists to prevent.

// The Trusted Types policy that youtube.com/tv's CSP (`require-trusted-types-for 'script'`) requires
// for any `.innerHTML` assignment used to live here as `js_trusted_html`. It is now folded into the
// two scripts that inject HTML — config/scripts/{error_page,overlay}.js — so each is self-contained;
// the toast uses `textContent` and needs no policy.

// Build the JS for one toast (config/scripts/toast.js rendered with text/ms params). The node is
// reused across calls (one id), auto-fades after `ms`, and is `pointer-events:none` so it can never
// swallow a click meant for Leanback.
std::string toast_js(std::string_view text, int ms);

// JS that removes the toast immediately (used when a state ends earlier than the timeout).
std::string toast_hide_js();

// Fire-and-forget: a failed toast must never break the thing it was announcing.
void show_toast(DevToolsClient& client, std::string_view text, int ms);
void hide_toast(DevToolsClient& client);

}  // namespace deckback
