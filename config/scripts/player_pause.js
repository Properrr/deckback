// Suspend checkpoint: pause and return currentTime, or -1 when there is no video. No params.
// Contains `pause()`, which the test double keys on.
(function (p) {
  var v = document.querySelector('video');
  if (!v) return -1;
  v.pause();
  return v.currentTime;
})
