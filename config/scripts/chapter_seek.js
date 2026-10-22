// Chapter-aware seek. p.dir is +1 (next) / -1 (prev); p.skip is the fixed-interval fallback in
// seconds. Chapter starts come from InnerTube /next (macroMarkersListEntity), cached per video on
// window.__dbChapters. Non-blocking: a cache miss starts the fetch and fixed-skips this press.
(function (p) {
  // Off the watch route the last video's player is still in the DOM, backgrounded; seeking it would
  // resume playback out of view.
  if (location.hash.indexOf('/watch') < 0) return false;
  // On TVHTML5 the player is `.html5-video-player`, NOT the desktop-only `#movie_player`.
  var pl = document.querySelector('.html5-video-player') || document.querySelector('#movie_player');
  if (!pl || typeof pl.seekTo !== 'function' || typeof pl.getCurrentTime !== 'function') {
    var v0 = document.querySelector('video');
    if (v0) { v0.currentTime = Math.max(0, v0.currentTime + (p.dir > 0 ? 1 : -1) * p.skip); return true; }
    return false;
  }
  var t = pl.getCurrentTime();
  var vid = null;
  try { vid = pl.getVideoData().video_id; } catch (e) {}

  var fixedSkip = function () {
    pl.seekTo(Math.max(0, t + (p.dir > 0 ? 1 : -1) * p.skip), true);
    return true;
  };

  var cache = window.__dbChapters;

  // Warm cache for this video: seconds, sorted ascending, starts[0] === 0.
  if (cache && cache.vid === vid && Array.isArray(cache.starts)) {
    var starts = cache.starts;
    if (!starts.length) return fixedSkip();
    if (p.dir > 0) {
      // Next boundary strictly ahead; the last chapter has none -> fixed skip forward.
      for (var i = 0; i < starts.length; i++) {
        if (starts[i] > t + 0.5) { pl.seekTo(starts[i], true); return true; }
      }
      return fixedSkip();
    } else {
      // Prev: the current chapter's start, unless within ~2s of it, then the previous chapter.
      var idx = 0;
      for (var j = 0; j < starts.length; j++) { if (starts[j] <= t + 0.001) idx = j; }
      if (t - starts[idx] < 2 && idx > 0) idx--;
      pl.seekTo(starts[idx], true);
      return true;
    }
  }

  // Cache miss (or a new video): kick off a background fetch, then fixed-skip this press.
  if (!cache || cache.vid !== vid) {
    window.__dbChapters = { vid: vid, starts: null };  // null = pending (don't re-kick)
    try {
      var g = window.ytcfg;
      var key = g.get('INNERTUBE_API_KEY');
      var ctx = g.get('INNERTUBE_CONTEXT');
      fetch('/youtubei/v1/next?key=' + encodeURIComponent(key) + '&prettyPrint=false',
        { method: 'POST', headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ context: ctx, videoId: vid }) })
        .then(function (r) { return r.json(); })
        .then(function (j) {
          var starts = [];
          try {
            var muts = j.frameworkUpdates.entityBatchUpdate.mutations || [];
            for (var i = 0; i < muts.length; i++) {
              var e = muts[i].payload && muts[i].payload.macroMarkersListEntity;
              if (e && e.markersList && e.markersList.markerType === 'MARKER_TYPE_CHAPTERS') {
                starts = (e.markersList.markers || []).map(function (m) {
                  return parseInt(m.startMillis, 10) / 1000;
                }).filter(function (x) { return !isNaN(x); });
                break;
              }
            }
          } catch (e) {}
          starts.sort(function (a, b) { return a - b; });
          window.__dbChapters = { vid: vid, starts: starts };
        })
        .catch(function () { window.__dbChapters = { vid: vid, starts: [] }; });
    } catch (e) {
      window.__dbChapters = { vid: vid, starts: [] };
    }
  }
  return fixedSkip();
})
