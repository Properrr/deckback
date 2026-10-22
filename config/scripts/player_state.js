// Play-state poll as a bitmask: bit0 playing, bit1 player open (#/watch), bit2 text input focused.
// No params. Keyed on `activeElement`, which appears only here (the test double matches on it).
(function (p) {
  var v = document.querySelector('video');
  var playing = !!(v && !v.paused && !v.ended && v.readyState > 2);
  var open = !!v && location.hash.indexOf('/watch') >= 0;
  var a = document.activeElement;
  var t = !!(a && (a.isContentEditable || /^(input|textarea)$/i.test(a.tagName || '')));
  return (playing ? 1 : 0) | (open ? 2 : 0) | (t ? 4 : 0);
})
