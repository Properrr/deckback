// Immediately fade the active toast (findings input-ux §14). No params.
(function (p) {
  var n = document.getElementById('__deckback_toast');
  if (window.__deckbackToastT) clearTimeout(window.__deckbackToastT);
  if (n) n.style.opacity = '0';
  return true;
})
