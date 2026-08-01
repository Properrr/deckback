// Hide the X cursor. A SEPARATE, SEPARATELY-INJECTED script on purpose — see the warning below.
//
// gamescope composites our X cursor, so `cursor: none` on the page makes it draw nothing (verified
// on-Deck). `disable_touch` gets this for free because `no_pointer.js` carries the same code; the
// gesture policy does not, because the two are mutually exclusive, so with the router installed
// no_pointer.js is not and the cursor comes back.
//
// ★ WHY IT IS NOT PART OF touch_gestures.js. It was, for one build, and on that build a real finger
// produced ZERO events of any family. The cause is NOT established — that build also changed the
// entire engine binary (gold+ThinLTO vs the deck/qa builds every hand test used), and the router's
// listeners were observed still attached — but the two suspects were entangled in one file, so
// neither could be dropped without dropping the other. Splitting them makes the question answerable
// on hardware: `touch_hide_cursor: false` in user.json removes exactly this and nothing else, with
// no rebuild. Keep it separate until `durable/touch-gestures.md` §7.0 closes.
//
// Page constraints, each learned the hard way: a `<style>` tag is blocked by youtube.com/tv's CSP
// (style-src has no 'unsafe-inline'), so this uses a CONSTRUCTABLE stylesheet with `insertRule` as a
// fallback and an inline baseline on documentElement that inherits down. The `*` rule beats
// per-element cursors like a button's `cursor: pointer`.
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
