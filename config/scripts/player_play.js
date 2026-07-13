// Resume nudge (player.cpp on_resume): resume if paused; returns whether a video element was present.
// Contains `play()`, which the test double keys on. No params.
(function (p) {
  var v = document.querySelector('video');
  if (v && v.paused) { v.play(); }
  return !!v;
})
