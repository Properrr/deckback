// AV1 steering (AV1 hardware decode is unproven on the Deck), injected at document-start: report AV1
// unsupported via MediaSource.isTypeSupported, canPlayType and mediaCapabilities.decodingInfo. Each
// hook is try/caught on its own so a missing API disables only that hook.
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
