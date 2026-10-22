// Hide the X cursor: gamescope composites it, so page-level `cursor: none` hides it. Separate from
// touch_gestures.js so `touch_hide_cursor: false` removes exactly this (no_pointer.js has its own
// copy). <style> is CSP-blocked: constructable sheet, insertRule fallback, inline baseline.
(function () {
  var W = window;
  var RULE = '*,*::before,*::after{cursor:none !important}';

  var hide = function () {
    var d = W.document;
    if (!d) return;
    // Inline baseline first: it needs no sheet machinery and survives a failed sheet install.
    try { d.documentElement.style.setProperty('cursor', 'none', 'important'); } catch (_) {}
    try {
      if (W.CSSStyleSheet && 'adoptedStyleSheets' in d && !W.__dbCursorSheet) {
        var sheet = new W.CSSStyleSheet();
        sheet.replaceSync(RULE);
        d.adoptedStyleSheets = [].concat(d.adoptedStyleSheets || [], sheet);
        W.__dbCursorSheet = sheet;
        return;
      }
    } catch (_) {}
    // Fallback for an engine without constructable stylesheets: append a rule to an existing sheet.
    try {
      var sheets = W.document.styleSheets;
      if (sheets && sheets.length) sheets[0].insertRule(RULE, sheets[0].cssRules.length);
    } catch (_) {}
  };

  hide();
  // Leanback replaces documentElement on navigation, taking the inline baseline with it.
  try { W.document.addEventListener('DOMContentLoaded', hide); } catch (_) {}
})();
