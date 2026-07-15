// Remove the update indicator (findings durable/self-update.md). Drops it from the keep-alive
// registry FIRST so the observer doesn't re-append it, then removes it. Ignores p, per convention.
(function (p) {
  var d = document.getElementById('__deckback_update_dot');
  if (d) {
    if (window.__dbDropAlive) window.__dbDropAlive(d);
    d.remove();
  }
  return true;
})
