// Resume nudge (player.cpp on_resume): resume if paused; returns whether a video element was present.
// Contains `play()`, which the test double keys on. No params.
(function (p) {
  // Only nudge playback while the watch view is the foreground route. Off it (e.g. resumed back on
  // the home screen) the last video's player still lives in the DOM but is backgrounded, and playing
  // it resumes hidden playback — the previous video plays audibly out of view. Mirror
  // player_state.js's own "player open" signal (location.hash contains '/watch').
  if (location.hash.indexOf('/watch') < 0) return false;
  var v = document.querySelector('video');
  if (v && v.paused) { v.play(); }
  return !!v;
})
