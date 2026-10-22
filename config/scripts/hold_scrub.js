// Press-and-hold playback rate: {"rate":2|0.5} on hold-start, {"stop":true} on release (no rewind:
// Chromium has no negative playbackRate). The rate snaps to the player's advertised ladder; stop
// restores the pre-hold rate. Returns the resulting <video>.playbackRate, or false.
(function (p) {
  var v = document.querySelector('video');
  if (!v) return false;
  var st = window.__deckbackScrub;

  if (!p || p.stop) {
    // Restore the pre-hold rate, not a hardcoded 1: the user may have set a speed of their own.
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
