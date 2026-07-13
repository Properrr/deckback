// Suspend checkpoint (player.cpp on_suspend): pause and return the current position so we can log a
// checkpoint; -1 when there is no video. Contains `pause()`, which the test double keys on. No params.
(function (p) {
  var v = document.querySelector('video');
  if (!v) return -1;
  v.pause();
  return v.currentTime;
})
