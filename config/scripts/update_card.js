// "Update available" card: a modal, launcher-owned overlay over Leanback. input.cpp owns the A/B/Y
// buttons (the card is modal there), so this file is presentational. Notes come from GitHub release
// bodies (untrusted) and are HTML-escaped before innerHTML (Trusted Types passes markup verbatim, it
// does not sanitise; policy memoised on window.__dbTTP). Self-healed across in-page body swaps by the
// keep-alive observer so the card can't vanish while input.cpp still treats it as modal (the "keys
// don't work" input trap, self-update.md). CSP style-src has no 'unsafe-inline', so a style element
// is dropped: descendant rules go in a constructable adoptedStyleSheets sheet (CSP-exempt, per
// no_pointer.js) and the container also gets a CSSOM fallback so the backdrop always paints.
// Params: heading, version, notes, and either buttons ([key,label] pairs → colour-coded hotkeys) or
// a plain footer string.
(function (p) {
  function esc(s) {
    return String(s == null ? '' : s).replace(/[&<>]/g, function (c) {
      return c === '&' ? '&amp;' : c === '<' ? '&lt;' : '&gt;';
    });
  }

  // Shared keep-alive (identical to update_badge.js so each script stays standalone).
  if (!window.__dbKeepAlive) {
    window.__dbKeep = window.__dbKeep || [];
    window.__dbKeepAlive = function (node) {
      if (window.__dbKeep.indexOf(node) === -1) window.__dbKeep.push(node);
      if (!window.__dbKeepObs && window.MutationObserver && document.documentElement) {
        try {
          window.__dbKeepObs = new MutationObserver(function () {
            var r = window.__dbKeep || [];
            for (var i = 0; i < r.length; i++) {
              try {
                if (r[i] && !r[i].isConnected) document.documentElement.appendChild(r[i]);
              } catch (_) {}
            }
          });
          window.__dbKeepObs.observe(document.documentElement, { childList: true });
        } catch (_) {}
      }
    };
    window.__dbDropAlive = function (node) {
      var r = window.__dbKeep || [];
      var i = r.indexOf(node);
      if (i !== -1) r.splice(i, 1);
    };
  }

  var RULES = "#__deckback_update{position:fixed;inset:0;z-index:2147483646;" +
    "background:rgba(8,8,8,0.94);color:#fff;display:flex;flex-direction:column;" +
    "align-items:center;justify-content:center;gap:16px;padding:6vh 8vw;box-sizing:border-box;" +
    "font:400 22px/1.4 system-ui,Roboto,Arial,sans-serif;}" +
    "#__deckback_update h2{margin:0;font-size:38px;font-weight:600;color:#f5b301;}" +
    "#__deckback_update .ver{font-size:24px;color:#9fd0ff;}" +
    "#__deckback_update .notes{max-width:900px;max-height:44vh;overflow:auto;text-align:left;" +
    "white-space:pre-wrap;background:rgba(255,255,255,0.06);border-radius:12px;" +
    "padding:16px 22px;font-size:20px;color:#eee;}" +
    "#__deckback_update .f{color:#bdbdbd;font-size:20px;}" +
    // A/B/Y hotkeys in Steam Deck face-button colours (must be a class rule, not an inline colour).
    "#__deckback_update .f .kb{font-weight:700;font-size:23px;}" +
    "#__deckback_update .f .kb-a{color:#3fb950;}" +
    "#__deckback_update .f .kb-b{color:#ff5c5c;}" +
    "#__deckback_update .f .kb-x{color:#5aa9ff;}" +
    "#__deckback_update .f .kb-y{color:#f5c531;}" +
    "#__deckback_update .f .kb-d{color:#9fd0ff;}" +
    "#__deckback_update .f .lbl{color:#cfcfcf;}" +
    "#__deckback_update .f .sep{color:#666;margin:0 14px;}";
  try {
    if (window.CSSStyleSheet && 'adoptedStyleSheets' in document && !window.__dbCardSheet) {
      window.__dbCardSheet = new CSSStyleSheet();
      window.__dbCardSheet.replaceSync(RULES);
      document.adoptedStyleSheets = [].concat(document.adoptedStyleSheets || [], window.__dbCardSheet);
    }
  } catch (e) {}

  // Footer: p.buttons ([key,label] pairs) → colour-coded glyphs; else the plain p.footer string.
  function keyClass(k) {
    k = String(k == null ? '' : k).toLowerCase();
    return (k === 'a' || k === 'b' || k === 'x' || k === 'y') ? ('kb-' + k) : 'kb-d';
  }
  var footerHtml;
  if (p.buttons && p.buttons.length) {
    var parts = [];
    for (var i = 0; i < p.buttons.length; i++) {
      var k = p.buttons[i][0], lbl = p.buttons[i][1];
      parts.push("<span class='kb " + keyClass(k) + "'>" + esc(k) + "</span> " +
        "<span class='lbl'>" + esc(lbl) + "</span>");
    }
    footerHtml = parts.join("<span class='sep'>·</span>");
  } else {
    footerHtml = esc(p.footer);
  }
  var html = "<h2>" + esc(p.heading) + "</h2>" +
    "<div class='ver'>" + esc(p.version) + "</div>" +
    "<div class='notes'>" + esc(p.notes) + "</div>" +
    "<div class='f'>" + footerHtml + "</div>";
  var h = html;
  try {
    var T = window.trustedTypes;
    if (T) {
      if (!window.__dbTTP)
        window.__dbTTP = T.createPolicy('deckback', { createHTML: function (x) { return x; } });
      h = window.__dbTTP.createHTML(html);
    }
  } catch (e) { h = html; }
  var id = '__deckback_update';
  var old = document.getElementById(id);
  if (old) {
    if (window.__dbDropAlive) window.__dbDropAlive(old);
    old.remove();
  }
  var d = document.createElement('div');
  d.id = id;
  d.innerHTML = h;
  // CSSOM fallback so the backdrop covers the page even if constructable stylesheets are missing.
  var s = d.style;
  s.setProperty('position', 'fixed');
  s.setProperty('inset', '0');
  s.setProperty('z-index', '2147483646');
  s.setProperty('display', 'flex');
  s.setProperty('flex-direction', 'column');
  s.setProperty('align-items', 'center');
  s.setProperty('justify-content', 'center');
  s.setProperty('gap', '16px');
  s.setProperty('padding', '6vh 8vw');
  s.setProperty('box-sizing', 'border-box');
  s.setProperty('background', 'rgba(8,8,8,0.94)');
  s.setProperty('color', '#fff');
  s.setProperty('font', '400 22px/1.4 system-ui,Roboto,Arial,sans-serif');
  document.documentElement.appendChild(d);
  window.__dbKeepAlive(d);
  return true;
})
