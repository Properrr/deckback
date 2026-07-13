// Voice unduck = resume (pairs with voice_pause.js). /*voice*/ marker as above. No params.
(function (p) { /*voice*/
  var v = document.querySelector('video');
  if (v && v.paused) { v.play(); return true; }
  return false;
})
