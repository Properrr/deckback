// On-screen toast (findings input-ux §14). p.text is shown for p.ms then fades. documentElement,
// not body: Leanback swaps body content on navigation, so an <html>-level child survives longer. The
// node is reused by id so repeated toasts do not stack. textContent (not innerHTML) needs no Trusted
// Types. A one-shot: ScriptLibrary.render() appends {"text":...,"ms":...}. Styled via CSSOM
// (.style.setProperty): youtube.com/tv's CSP style-src has no 'unsafe-inline', so an inline style
// attribute is dropped and the toast renders unstyled (durable/self-update.md).
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
  // pre-wrap, not pre: `pre` never wraps, so a toast wider than the panel is clipped at BOTH edges
  // (centred via translateX(-50%)) with no ellipsis and no error. max-width makes it wrap early.
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
