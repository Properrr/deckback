// On-screen toast: shows p.text for p.ms, then fades. One node, reused by id, on documentElement
// (Leanback swaps body content). textContent, never innerHTML (Trusted Types); styled via CSSOM
// because youtube.com/tv's CSP drops inline style attributes.
(function (p) {
  var id = '__deckback_toast';
  var d = document.getElementById(id);
  if (!d) { d = document.createElement('div'); d.id = id; document.documentElement.appendChild(d); }
  d.textContent = p.text;
  var s = d.style;
  s.setProperty('position', 'fixed');
  s.setProperty('left', '50%');
  s.setProperty('top', '7%');
  s.setProperty('transform', 'translateX(-50%)');
  s.setProperty('z-index', '2147483647');
  s.setProperty('background', 'rgba(0,0,0,0.86)');
  s.setProperty('color', '#fff');
  s.setProperty('font', '600 30px/1.25 system-ui,sans-serif');
  s.setProperty('padding', '18px 30px');
  s.setProperty('border-radius', '14px');
  s.setProperty('pointer-events', 'none');
  // pre-wrap + max-width: `pre` would clip a wide toast at both edges instead of wrapping.
  s.setProperty('white-space', 'pre-wrap');
  s.setProperty('max-width', '76vw');
  s.setProperty('text-align', 'center');
  s.setProperty('opacity', '1');
  s.setProperty('transition', 'opacity .35s ease-out');
  if (window.__deckbackToastT) clearTimeout(window.__deckbackToastT);
  var ms = p.ms < 0 ? 0 : p.ms;
  window.__deckbackToastT = setTimeout(function () {
    var n = document.getElementById(id); if (n) n.style.opacity = '0';
  }, ms);
  return true;
})
