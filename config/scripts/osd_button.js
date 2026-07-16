// Settings button (osd-menu-plan.md §6): top-right pill affordance for the OSD menu, replacing the
// self-update pill. pointer-events:none, CSSOM-styled, keep-alive'd. Params: label, badge (bool).
(function (p) {
  // keep-alive (shared)
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

  function css(elm, o) { for (var k in o) elm.style.setProperty(k, o[k]); }

  var label = (p && p.label) ? p.label : 'Settings';
  var showBadge = !!(p && p.badge);

  var id = '__deckback_settings_btn';
  var box = document.getElementById(id);
  if (!box) {
    box = document.createElement('div');
    box.id = id;
    document.documentElement.appendChild(box);
  }
  while (box.firstChild) box.removeChild(box.firstChild);  // rebuild children, idempotent
  box.setAttribute('title', label);
  css(box, {
    position: 'fixed', top: '10px', right: '14px', 'z-index': '2147483645',
    'pointer-events': 'none', display: 'flex', 'align-items': 'center', gap: '9px',
    background: 'rgba(10,10,10,0.72)', 'border-radius': '999px', padding: '7px 13px',
    font: '600 20px/1 system-ui,Roboto,Arial,sans-serif', color: '#fff',
    'box-shadow': '0 2px 10px rgba(0,0,0,0.45)'
  });

  var gear = document.createElement('span');
  gear.textContent = '⚙';  // ⚙ gear
  css(gear, { 'font-size': '22px', 'line-height': '1', flex: '0 0 auto' });
  var text = document.createElement('span');
  text.textContent = label;
  box.appendChild(gear);
  box.appendChild(text);

  if (showBadge) {
    var dot = document.createElement('span');
    css(dot, {
      width: '11px', height: '11px', 'border-radius': '50%', background: '#f5b301',
      'box-shadow': '0 0 6px 2px rgba(245,179,1,0.55)', flex: '0 0 auto'
    });
    box.appendChild(dot);
  }

  if (window.__dbKeepAlive) window.__dbKeepAlive(box);
  return true;
})
