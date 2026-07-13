// Voice unduck = unmute (pairs with voice_mute.js). /*voice*/ marker as above. No params.
(function (p) { /*voice*/
  var v = document.querySelector('video');
  if (v && v.muted) { v.muted = false; return true; }
  return false;
})
