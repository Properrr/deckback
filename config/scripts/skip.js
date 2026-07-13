// Fixed-interval player seek (findings input-ux §18). Bound to L2/R2 via the keymap actions
// skip_back/skip_fwd; p.delta is the signed jump in seconds (negative = back). Uses the player's own
// PUBLIC seekBy() (YouTube IFrame-API parity — not internal-JSON scraping) and falls back to the raw
// <video> element, clamped at 0. Returns whether it seeked. A one-shot: ScriptLibrary.render() appends
// the params object, so this file is the function only.
(function (p) {
  var d = p.delta;
  var pl = document.querySelector('#movie_player');
  if (pl && typeof pl.seekBy === 'function') { pl.seekBy(d, true); return true; }
  var v = document.querySelector('video');
  if (v) { v.currentTime = Math.max(0, v.currentTime + d); return true; }
  return false;
})
