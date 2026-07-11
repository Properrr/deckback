#pragma once
#include <string>
#include <string_view>

#include "devtools.hpp"

namespace deckback {

// A CDP-injected DOM toast. The engine has no overlay surface of its own and we refuse to patch one
// in, so on-screen feedback is a <div> we append to `document.documentElement` over CDP. This is
// the same technique `navigator.cpp` already uses for codec steering — no engine patch, and it
// survives a Leanback navigation because we re-inject rather than persist.
//
// It exists because the touch lock is otherwise **unobservable**: findings input-ux §4 requires a
// glyph/toast on engage, and today a user who trips the chord sees a screen that has simply stopped
// responding to touch. That is the bug report this exists to prevent.

// Escape for embedding inside a **double-quoted JS string literal**. This is NOT the CDP JSON
// escaping — devtools.cpp applies that on top, and the two must not be confused: a `\"` produced
// here becomes `\\\"` on the wire, which is correct. Shared with errorpage.cpp, which builds its
// injected document the same way.
std::string js_string_escape(std::string_view s);

// A JS *expression* that turns `raw_expr` (itself a JS expression evaluating to an HTML string)
// into a value assignable to `.innerHTML` under a Trusted Types CSP, falling back to the raw string
// where Trusted Types is absent.
//
// youtube.com/tv enforces `require-trusted-types-for 'script'`, so `el.innerHTML = "<h2>..."`
// throws `This document requires 'TrustedHTML' assignment` and the assignment silently does
// nothing. That is why the controls card rendered in every host-side test and NOTHING on the real
// page (verified on-Deck 2026-07-10): the account gate had been masking Leanback the whole time, so
// no earlier run ever met the CSP. `textContent` is unaffected — the toast, which uses it, was
// always fine.
//
// The policy is memoised on `window` and its creation is wrapped in try/catch: a page whose CSP
// pins a `trusted-types` allowlist without our name would otherwise throw on createPolicy, and the
// correct degraded behaviour is a plain string (which throws visibly on the real page, never a
// silent success). about:blank has no Trusted Types, so the error page keeps working there too.
std::string js_trusted_html(std::string_view raw_expr);

// Build the JS for one toast. Pure, so the escaping is testable: `text` lands inside a JS string
// literal and a stray quote or backslash would otherwise produce a syntax error and a silent no-op.
// The node is reused across calls (one id), auto-fades after `ms`, and is `pointer-events:none` so
// it can never swallow a click meant for Leanback.
std::string toast_js(std::string_view text, int ms);

// JS that removes the toast immediately (used when a state ends earlier than the timeout).
std::string toast_hide_js();

// Fire-and-forget: a failed toast must never break the thing it was announcing.
void show_toast(DevToolsClient& client, std::string_view text, int ms);
void hide_toast(DevToolsClient& client);

}  // namespace deckback
