// AV1 codec steering. THE source of truth: compiled into the launcher (CMake generates
// av1_steering_js.hpp from this file) and injected verbatim by `just smoke`, so the nightly canary
// exercises the script that ships rather than a copy of it that can drift.
//
// Why steer at all: AV1 hardware decode on the Deck is *unproven through* VaapiVideoDecoder. libva
// on the OLED unit advertises AV1Profile0, so the old "no AV1 hardware" claim is disputed
// (.internal/findings/durable/hardware.md) — but until we observe AV1 decoding through the platform
// path we assume a Dav1d software fallback, which blows the battery budget.
//
// Injected at the start of every document (Page.addScriptToEvaluateOnNewDocument), before Leanback
// probes formats. Covers the three APIs YouTube TV uses to choose a codec. Done over CDP rather
// than as an engine patch, per the launcher-as-platform-layer principle.
//
// Each override is independently try/caught: a missing API must disable that one hook, never abort
// the other two and leave AV1 half-steered.
(function(){
  var AV1 = /av01|(^|[^a-z])av1([^a-z]|$)/i;
  var isAv1 = function(s){ return typeof s === 'string' && AV1.test(s); };
  try {
    if (window.MediaSource && MediaSource.isTypeSupported) {
      var mse = MediaSource.isTypeSupported.bind(MediaSource);
      MediaSource.isTypeSupported = function(t){ return isAv1(t) ? false : mse(t); };
    }
  } catch(e){}
  try {
    if (window.HTMLMediaElement && HTMLMediaElement.prototype.canPlayType) {
      var cpt = HTMLMediaElement.prototype.canPlayType;
      HTMLMediaElement.prototype.canPlayType = function(t){ return isAv1(t) ? '' : cpt.call(this, t); };
    }
  } catch(e){}
  try {
    if (navigator.mediaCapabilities && navigator.mediaCapabilities.decodingInfo) {
      var di = navigator.mediaCapabilities.decodingInfo.bind(navigator.mediaCapabilities);
      navigator.mediaCapabilities.decodingInfo = function(cfg){
        try {
          var ct = cfg && cfg.video && cfg.video.contentType;
          if (isAv1(ct)) return Promise.resolve({supported:false, smooth:false, powerEfficient:false});
        } catch(e){}
        return di(cfg);
      };
    }
  } catch(e){}
})();
