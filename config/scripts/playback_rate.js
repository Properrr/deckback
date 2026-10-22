// Set the playback rate to p.rate, snapped to the player's advertised ladder (a rate outside it is
// ignored or stalls). Calls the player's setPlaybackRate, then assigns <video>.playbackRate, which
// wins. Returns the resulting rate, or false.
(function (p) {
  var rate = Number(p && p.rate);
  if (!isFinite(rate) || rate <= 0) return false;

  var el = document.querySelector('.html5-video-player');
  var v = document.querySelector('video');
  if (!v) return false;

  try {
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
  } catch (_) {}

  try { if (el && typeof el.setPlaybackRate === 'function') el.setPlaybackRate(rate); } catch (_) {}
  try { v.playbackRate = rate; } catch (_) { return false; }
  return v.playbackRate;
})
