// Kiosk failure page: replaces Chromium's interstitial with a focused Try-again button. Params: title,
// hint, url, detail (strings). Enter/Space/click set window.__deckbackRetry; no key quits. innerHTML
// goes through a Trusted Types policy (memoised on window.__dbTTP); raw string on about:blank.
(function (p) {
  var STYLE = "<style>html,body{margin:0;height:100%;background:#0f0f0f;color:#fff;" +
    "font:400 24px/1.45 system-ui,Roboto,Arial,sans-serif;}" +
    "#__deckback_error{box-sizing:border-box;height:100%;display:flex;flex-direction:column;" +
    "align-items:center;justify-content:center;text-align:center;padding:6vh 8vw;gap:14px;}" +
    "h1{margin:0;font-size:44px;font-weight:600;letter-spacing:-.01em;}" +
    "p{margin:0;max-width:26em;color:#c9c9c9;}" +
    "#__deckback_retry{margin-top:22px;font:600 26px/1 system-ui,sans-serif;color:#0f0f0f;" +
    "background:#fff;border:0;border-radius:999px;padding:18px 44px;cursor:pointer;}" +
    "#__deckback_retry:focus{outline:4px solid #3ea6ff;outline-offset:4px;}" +
    "small{color:#7a7a7a;font-size:15px;word-break:break-all;max-width:34em;}</style>";
  var html = STYLE + "<body><div id='__deckback_error'>" +
    "<h1>" + p.title + "</h1><p>" + p.hint + "</p>" +
    "<button id='__deckback_retry'>Try again</button>" +
    "<small>" + p.url + "</small><small>" + p.detail + "</small>" +
    "</div></body>";
  var h = html;
  try {
    var T = window.trustedTypes;
    if (T) {
      if (!window.__dbTTP)
        window.__dbTTP = T.createPolicy('deckback', { createHTML: function (x) { return x; } });
      h = window.__dbTTP.createHTML(html);
    }
  } catch (e) { h = html; }
  document.documentElement.innerHTML = h;
  window.__deckbackRetry=false;
  var b = document.getElementById('__deckback_retry');
  if (b) { b.focus(); b.addEventListener('click', function () { window.__deckbackRetry = true; }); }
  document.addEventListener('keydown', function (e) {
    if (e.key==='Enter' || e.key===' ' || e.key==='Spacebar') window.__deckbackRetry = true;
  });
  return true;
})
