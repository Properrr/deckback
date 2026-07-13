// Voice duck = pause (findings input-ux §13.3): removes speaker bleed entirely before the mic opens.
// The /*voice*/ marker keeps this apart from player_pause.js (both call pause()); the test double
// keys on it. No params.
(function (p) { /*voice*/
  var v = document.querySelector('video');
  if (v && !v.paused) { v.pause(); return true; }
  return false;
})
