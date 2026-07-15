// "Update available" indicator: a pill (amber dot + label + ☰ keycap) pinned top-right, telling the
// user the card is on the Menu (☰) button. Launcher-drawn, pointer-events:none, self-healed across
// Leanback body swaps by the shared keep-alive observer. youtube.com/tv's CSP style-src has no
// 'unsafe-inline', so styling is CSSOM-only (.style.setProperty) — inline style attrs and <style>
// tags are dropped (durable/self-update.md, touch-lock.md). Params (optional): label, hint.
(function (p) {
  // Shared keep-alive: re-append our nodes if a Leanback body swap detaches them (an in-page swap
  // doesn't fire the navigator's on_app_loaded). Defined once; identical in update_card.js.
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

  function css(el, o) { for (var k in o) el.style.setProperty(k, o[k]); }

  var label = (p && p.label) ? p.label : 'Update available';
  var hint = (p && p.hint) ? p.hint : '☰';

  var id = '__deckback_update_dot';
  var box = document.getElementById(id);
  if (!box) {
    box = document.createElement('div');
    box.id = id;
    document.documentElement.appendChild(box);
  }
  while (box.firstChild) box.removeChild(box.firstChild);  // rebuild children, idempotent
  box.setAttribute('title', label);
  css(box, {
    position: 'fixed', top: '10px', right: '14px', 'z-index': '2147483647',
    'pointer-events': 'none', display: 'flex', 'align-items': 'center', gap: '10px',
    background: 'rgba(10,10,10,0.72)', 'border-radius': '999px', padding: '7px 12px',
    font: '600 20px/1 system-ui,Roboto,Arial,sans-serif', color: '#fff',
    'box-shadow': '0 2px 10px rgba(0,0,0,0.45)'
  });

  var dot = document.createElement('span');
  css(dot, {
    width: '12px', height: '12px', 'border-radius': '50%', background: '#f5b301',
    'box-shadow': '0 0 6px 2px rgba(245,179,1,0.55)', flex: '0 0 auto'
  });
  var text = document.createElement('span');
  text.textContent = label;
  var cap = document.createElement('span');
  cap.textContent = hint;
  css(cap, {
    display: 'inline-flex', 'align-items': 'center', 'justify-content': 'center',
    'min-width': '30px', height: '28px', padding: '0 6px', 'border-radius': '7px',
    border: '2px solid #9aa0a6', color: '#fff', 'font-size': '18px'
  });
  box.appendChild(dot);
  box.appendChild(text);
  box.appendChild(cap);

  window.__dbKeepAlive(box);
  return true;
})
