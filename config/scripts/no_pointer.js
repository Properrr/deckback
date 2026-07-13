// Touch/pointer suppression AND cursor hiding. THE source of truth: the launcher compiles this in (CMake generates
// no_pointer_js.hpp from this file) and injects it at document-start
// (Page.addScriptToEvaluateOnNewDocument) when config `disable_touch` is set. Embedding it — rather
// than reading it at runtime — keeps a missing/unreadable file from silently re-enabling touch.
//
// Why: on the Steam Deck under gamescope the touchscreen is delivered to us as synthetic MOUSE /
// POINTER events, never touch events (verified on-Deck: a finger produces pointerdown/click, never
// touchstart — findings durable/touch-lock.md). So a finger moves the cursor and a tap clicks,
// which navigates YouTube by accident — the bad experience this removes. Leanback TV is entirely
// D-pad / key driven and our gamepad input is injected as KEY events, so swallowing every
// pointer/mouse/touch event costs the user nothing and makes touch inert. This is the app-level
// half of the fix (Option A); the launcher also pins gamescope's global touch mode to hover
// (Option B, launcher/src/touchmode.cpp) as defense in depth.
//
// Mechanism: register CAPTURING listeners on window (and document) at document-start, so they run
// BEFORE any Leanback handler; stopImmediatePropagation() + preventDefault() kill the event before
// anything sees it. Registered at document-start, we are first in the capture chain.
//
// NOTE: this also swallows the synthetic mic-button click used by the (default-OFF, unverified)
// voice feature. disable_touch and voice_enabled are therefore mutually exclusive until voice is
// verified on hardware; disable_touch wins.
(function () {
  var KILL = [
    'pointerdown', 'pointerup', 'pointermove', 'pointercancel',
    'pointerover', 'pointerout', 'pointerenter', 'pointerleave',
    'gotpointercapture', 'lostpointercapture',
    'mousedown', 'mouseup', 'mousemove',
    'mouseover', 'mouseout', 'mouseenter', 'mouseleave',
    'click', 'dblclick', 'auxclick', 'contextmenu',
    'touchstart', 'touchend', 'touchmove', 'touchcancel'
  ];
  var swallow = function (e) {
    try { e.stopImmediatePropagation(); } catch (_) {}
    try { if (e.cancelable) e.preventDefault(); } catch (_) {}
  };
  var install = function (target) {
    if (!target || !target.addEventListener) return;
    for (var i = 0; i < KILL.length; i++) {
      try { target.addEventListener(KILL[i], swallow, { capture: true, passive: false }); } catch (_) {}
    }
  };
  // window is the top of the capture chain; document is belt-and-suspenders in case an engine
  // dispatches an event that does not traverse window.
  try { install(window); } catch (_) {}
  try { install(document); } catch (_) {}

  // Hide the cursor. gamescope composites OUR X cursor, so `cursor: none` on the page makes it draw
  // nothing (verified on-Deck: the cursor vanishes). Touch still moves an invisible pointer, but with
  // no visible cursor and no clicks the panel is fully inert. A `<style>` tag is blocked by
  // youtube.com/tv's CSP (style-src), so we use a CONSTRUCTABLE stylesheet (adoptedStyleSheets) — the
  // `*` rule beats per-element cursors like a button's `cursor: pointer` — with `insertRule` into an
  // existing sheet as a fallback, plus a documentElement inline baseline that inherits down.
  var RULE = '*,*::before,*::after{cursor:none !important}';
  var hideCursor = function () {
    try { document.documentElement.style.setProperty('cursor', 'none', 'important'); } catch (_) {}
    try {
      if (window.CSSStyleSheet && 'adoptedStyleSheets' in document && !window.__dbCursorSheet) {
        var sheet = new CSSStyleSheet();
        sheet.replaceSync(RULE);
        document.adoptedStyleSheets = [].concat(document.adoptedStyleSheets || [], sheet);
        window.__dbCursorSheet = sheet;
        return;
      }
    } catch (_) {}
    // Fallback: splice into an existing sheet (needs one to exist, hence the DOMContentLoaded retry).
    try { if (document.styleSheets[0]) document.styleSheets[0].insertRule(RULE, 0); } catch (_) {}
  };
  hideCursor();
  try {
    if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', hideCursor, { once: true, capture: true });
    }
  } catch (_) {}
})();
