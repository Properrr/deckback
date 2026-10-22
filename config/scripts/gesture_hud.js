// Touch-gesture indicator (seek ripple / rate pill). One-shot params {"text","side","ms"}: side is
// left|right|center; ms 0 pins it until hidden; empty text hides it. textContent and CSSOM only
// (Trusted Types, CSP style-src), on documentElement because Leanback swaps body content.
(function (p) {
  var id = '__deckback_gesture_hud';
  var d = document.getElementById(id);
  if (!d) {
    d = document.createElement('div');
    d.id = id;
    document.documentElement.appendChild(d);
  }

  if (window.__deckbackHudT) { clearTimeout(window.__deckbackHudT); window.__deckbackHudT = null; }

  var text = p && p.text ? String(p.text) : '';
  if (!text) {
    d.style.setProperty('opacity', '0');
    return true;
  }
  d.textContent = text;

  var s = d.style;
  s.setProperty('position', 'fixed');
  s.setProperty('top', '50%');
  s.setProperty('z-index', '2147483646');   // just under the toast
  s.setProperty('transform', 'translateY(-50%)');
  // Sit over the side that was touched.
  var side = p && p.side ? p.side : 'center';
  s.setProperty('left', side === 'left' ? '12%' : (side === 'right' ? 'auto' : '50%'));
  s.setProperty('right', side === 'right' ? '12%' : 'auto');
  if (side === 'center') s.setProperty('transform', 'translate(-50%,-50%)');

  s.setProperty('background', 'rgba(0,0,0,0.72)');
  s.setProperty('color', '#fff');
  s.setProperty('font', '600 34px/1.2 system-ui,sans-serif');
  s.setProperty('padding', '20px 34px');
  s.setProperty('border-radius', '999px');
  s.setProperty('pointer-events', 'none');
  s.setProperty('white-space', 'pre');
  s.setProperty('text-align', 'center');
  s.setProperty('opacity', '1');
  s.setProperty('transition', 'opacity .18s ease-out');

  var ms = p && typeof p.ms === 'number' ? p.ms : 900;
  if (ms > 0) {
    window.__deckbackHudT = setTimeout(function () {
      var n = document.getElementById(id);
      if (n) n.style.setProperty('opacity', '0');
    }, ms);
  }
  return true;
})
