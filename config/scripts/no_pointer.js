// Touch/pointer suppression and cursor hiding, injected at document-start when `disable_touch` is
// set. Capture-phase listeners swallow every pointer/mouse/touch event before Leanback sees it. This
// also swallows the voice feature's synthetic mic click, so disable_touch excludes voice_enabled.
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
  // document too, in case an event does not traverse window.
  try { install(window); } catch (_) {}
  try { install(document); } catch (_) {}

  // Hide the cursor (gamescope composites ours). <style> is CSP-blocked, so: constructable sheet,
  // insertRule fallback, inline documentElement baseline.
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
