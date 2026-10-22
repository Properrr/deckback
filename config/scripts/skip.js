// Fixed-interval seek, the fallback for chapter_seek.js. p.delta is the signed jump in seconds. Uses
// the player's seekBy(), else the raw <video> clamped at 0. Returns false when nothing was seeked.
(function (p) {
  // Off the watch route the last video's player is still in the DOM, backgrounded; seeking it would
  // resume playback out of view.
  if (location.hash.indexOf('/watch') < 0) return false;
  var d = p.delta;
  // On TVHTML5 the player is `.html5-video-player`, NOT the desktop-only `#movie_player`.
  var pl = document.querySelector('.html5-video-player') || document.querySelector('#movie_player');
  if (pl && typeof pl.seekBy === 'function') { pl.seekBy(d, true); return true; }
  var v = document.querySelector('video');
  if (v) { v.currentTime = Math.max(0, v.currentTime + d); return true; }
  return false;
})
