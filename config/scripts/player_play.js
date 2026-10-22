// Resume nudge: play() if paused; returns whether a video element was present. No params.
// Contains `play()`, which the test double keys on.
(function (p) {
  // Off the watch route the last video's player is still in the DOM, backgrounded; playing it would
  // play audibly out of view.
  if (location.hash.indexOf('/watch') < 0) return false;
  var v = document.querySelector('video');
  if (v && v.paused) { v.play(); }
  return !!v;
})
