// Remove the Settings button; drop from keep-alive first so a deliberate hide isn't fought.
(function () {
  var id = '__deckback_settings_btn';
  var n = document.getElementById(id);
  if (n) {
    if (window.__dbDropAlive) window.__dbDropAlive(n);
    n.remove();
  }
  return true;
})
