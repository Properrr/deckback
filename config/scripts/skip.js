// Fixed-interval player seek (findings input-ux §18), the fallback for chapter_seek.js. Bound to
// L2/R2 via the keymap actions skip_back/skip_fwd; p.delta is the signed jump in seconds. Uses the
// player's own public seekBy() and falls back to the raw <video>, clamped at 0.
(function (p) {
  // Only seek while the watch view is the foreground route. Off it (e.g. back on the home screen)
  // the last video's player still lives in the DOM but is backgrounded, and seeking it RESUMES that
  // hidden playback — the previous video plays audibly out of view. Mirror player_state.js's own
  // "player open" signal (location.hash contains '/watch') so the seek and the layer poll agree.
  if (location.hash.indexOf('/watch') < 0) return false;
  var d = p.delta;
  // On TVHTML5 the player is `.html5-video-player`, NOT the desktop-only `#movie_player`.
  var pl = document.querySelector('.html5-video-player') || document.querySelector('#movie_player');
  if (pl && typeof pl.seekBy === 'function') { pl.seekBy(d, true); return true; }
  var v = document.querySelector('video');
  if (v) { v.currentTime = Math.max(0, v.currentTime + d); return true; }
  return false;
})
