// Locate Leanback's soft-mic button (findings input-ux §13.2). Returns "x,y" (viewport centre in CSS
// px) for the first visible candidate in p.selectors, "" when none match. A zero-area rect means the
// element exists but is not laid out — clicking it would do nothing, so treat it as absent.
// querySelector throws on invalid selector syntax, so each lookup is try/caught. Contains
// getBoundingClientRect, which the test double keys on.
(function (p) {
  var sels = p.selectors;
  for (var i = 0; i < sels.length; i++) {
    var e = null;
    try { e = document.querySelector(sels[i]); } catch (err) {}
    if (!e) continue;
    var r = e.getBoundingClientRect();
    if (!r.width || !r.height) continue;
    return (r.left + r.width / 2) + ',' + (r.top + r.height / 2);
  }
  return '';
})
