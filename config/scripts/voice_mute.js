// Voice duck = mute (keeps the timeline moving; some users prefer it to pause). /*voice*/ marker as
// above. No params.
(function (p) { /*voice*/
  var v = document.querySelector('video');
  if (v && !v.muted) { v.muted = true; return true; }
  return false;
})
