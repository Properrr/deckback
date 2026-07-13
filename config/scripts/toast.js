// On-screen toast (findings input-ux §14). p.text is shown for p.ms then fades. documentElement,
// not body: Leanback swaps body content on navigation, so an <html>-level child survives longer. The
// node is reused by id so repeated toasts do not stack. textContent (not innerHTML) needs no Trusted
// Types. A one-shot: ScriptLibrary.render() appends {"text":...,"ms":...}.
(function (p) {
  var id = '__deckback_toast';
  var d = document.getElementById(id);
  if (!d) { d = document.createElement('div'); d.id = id; document.documentElement.appendChild(d); }
  d.textContent = p.text;
  d.setAttribute('style',
    'position:fixed;left:50%;top:7%;transform:translateX(-50%);z-index:2147483647;' +
    'background:rgba(0,0,0,0.86);color:#fff;font:600 30px/1.25 system-ui,sans-serif;' +
    'padding:18px 30px;border-radius:14px;pointer-events:none;white-space:pre;' +
    'opacity:1;transition:opacity .35s ease-out;');
  if (window.__deckbackToastT) clearTimeout(window.__deckbackToastT);
  var ms = p.ms < 0 ? 0 : p.ms;
  window.__deckbackToastT = setTimeout(function () {
    var n = document.getElementById(id); if (n) n.style.opacity = '0';
  }, ms);
  return true;
})
