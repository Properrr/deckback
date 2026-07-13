// First-run controls card (findings input-ux §17). Params: title, footer (strings) and rows (an
// array of [control, action] pairs — structured data the page turns into <td>s, so nothing is
// pre-escaped by the caller). Appended to documentElement (not innerHTML-replaced) because Leanback
// is still alive underneath. innerHTML needs a Trusted Types value under youtube.com/tv's CSP; the
// policy is folded in here (memoised on window.__dbTTP), which is why the card renders on the Deck
// and not only in host-side tests.
(function (p) {
  var STYLE = "<style>#__deckback_help{position:fixed;inset:0;z-index:2147483646;" +
    "background:rgba(8,8,8,0.93);color:#fff;display:flex;flex-direction:column;" +
    "align-items:center;justify-content:center;gap:22px;" +
    "font:400 24px/1.35 system-ui,Roboto,Arial,sans-serif;}" +
    "#__deckback_help h2{margin:0;font-size:40px;font-weight:600;}" +
    "#__deckback_help table{border-collapse:collapse;font-size:26px;}" +
    "#__deckback_help td{padding:9px 26px;}" +
    "#__deckback_help td.k{text-align:right;color:#9fd0ff;font-weight:600;white-space:nowrap;}" +
    "#__deckback_help td.v{text-align:left;color:#eee;}" +
    "#__deckback_help .f{color:#9a9a9a;font-size:20px;}</style>";
  var body = '';
  for (var i = 0; i < p.rows.length; i++) {
    body += "<tr><td class='k'>" + p.rows[i][0] + "</td><td class='v'>" + p.rows[i][1] + "</td></tr>";
  }
  var html = STYLE + "<h2>" + p.title + "</h2><table>" + body +
    "</table><div class='f'>" + p.footer + "</div>";
  var h = html;
  try {
    var T = window.trustedTypes;
    if (T) {
      if (!window.__dbTTP)
        window.__dbTTP = T.createPolicy('deckback', { createHTML: function (x) { return x; } });
      h = window.__dbTTP.createHTML(html);
    }
  } catch (e) { h = html; }
  var id = '__deckback_help';
  var old = document.getElementById(id);
  if (old) old.remove();
  var d = document.createElement('div');
  d.id = id;
  d.innerHTML = h;
  document.documentElement.appendChild(d);
  return true;
})
