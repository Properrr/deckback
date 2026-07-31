// Set the playback rate. Used by the touch gesture router's press-and-hold (mobile's
// hold-anywhere-for-2x) and available to any future speed binding.
//
// What P12.0b measured on-Deck (2026-07-31, feature-landscape §8.1), and what it did not:
//
//   * `<video>.playbackRate = 1.25` HELD through 3 s of real playback — the element is the proven
//     lever, so it is the one applied last and the one whose result is reported;
//   * the player exposes `setPlaybackRate` and `getAvailablePlaybackRates() -> [0.25 .. 2]`, but
//     `setPlaybackRate` was only shown to EXIST (a no-arg call throws). It is called first, so the
//     player's own state follows the rate where that works, and the element assignment still wins
//     if it does not.
//
// The rate is clamped to the ladder the player advertises where there is one: asking for a rate
// outside it is how you get a silently ignored call or a stall.
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
