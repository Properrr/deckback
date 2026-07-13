// Remove the first-run controls card (findings input-ux §17). No params.
(function (p) {
  var n = document.getElementById('__deckback_help');
  if (n) n.remove();
  return true;
})
