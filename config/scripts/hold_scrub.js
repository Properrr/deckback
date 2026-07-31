// Press-and-hold playback rate: hold the RIGHT third to speed up, the LEFT third to slow down, and
// release to go back to normal. A one-shot — the launcher renders {"rate":2} or {"rate":0.5} on
// hold-start and {"stop":true} on release.
//
// Both directions are the SAME mechanism, which is the point. An earlier design made the left third
// a rewind, and rewind is not a rate: Chromium has no negative `playbackRate` (assigning one throws
// or is ignored, and there is no decoder path for it), so it would have had to be stepped seeks on a
// timer — a different code path, a different feel, and a decoder thrash. Slow-motion is a real rate
// on the ladder the player already advertises, so left and right differ only in the number.
//
// P12.0b measured that ladder on-Deck: [0.25, 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2], and measured
// <video>.playbackRate holding through real playback. The requested rate is snapped to the nearest
// advertised value, because asking for one outside the ladder is how you get a silently ignored
// call or a stall.
(function (p) {
  var v = document.querySelector('video');
  if (!v) return false;
  var st = window.__deckbackScrub;

  if (!p || p.stop) {
    // Restore the rate the user was actually watching at, not a hardcoded 1: they may have set a
    // speed of their own, and a hold must not quietly reset it.
    if (st && typeof st.rate0 === 'number') {
      try { v.playbackRate = st.rate0; } catch (_) {}
      try {
        var e0 = document.querySelector('.html5-video-player');
        if (e0 && typeof e0.setPlaybackRate === 'function') e0.setPlaybackRate(st.rate0);
      } catch (_) {}
    }
    window.__deckbackScrub = null;
    return v.playbackRate;
  }

  var rate = Number(p.rate);
  if (!isFinite(rate) || rate <= 0) return false;

  // A second hold without a release must not capture the HELD rate as the one to restore to.
  if (!st) window.__deckbackScrub = st = {rate0: v.playbackRate};

  try {
    var el = document.querySelector('.html5-video-player');
    if (el && typeof el.getAvailablePlaybackRates === 'function') {
      var rates = el.getAvailablePlaybackRates();
      if (rates && rates.length) {
        var best = rates[0];
        for (var i = 0; i < rates.length; i++) {
          if (Math.abs(rates[i] - rate) < Math.abs(best - rate)) best = rates[i];
        }
        rate = best;
      }
    }
    if (el && typeof el.setPlaybackRate === 'function') el.setPlaybackRate(rate);
  } catch (_) {}

  try { v.playbackRate = rate; } catch (_) { return false; }
  return v.playbackRate;
})
