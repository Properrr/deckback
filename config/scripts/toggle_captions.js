// Caption selection and control through Leanback's player caption module.
(function (p) {
  if (location.hash.indexOf('/watch') < 0) return 'na';
  var pl = document.querySelector('.html5-video-player') || document.querySelector('#movie_player');
  if (!pl || typeof pl.getOption !== 'function' || typeof pl.setOption !== 'function') return 'na';

  var type = p.type;
  if (type !== 'author_first' && type !== 'auto_first' && type !== 'author_only' &&
      type !== 'auto_only') type = 'author_first';
  var sub = function (code) {
    return (code || '').toLowerCase().split(/[-_]/)[0];
  };
  var isAsr = function (track) {
    return !!(track && (track.kind === 'asr' || track.is_asr));
  };
  var isTrans = function (track) {
    return !!(track && track.translationLanguage);
  };
  var effLang = function (track) {
    return (track && track.translationLanguage && track.translationLanguage.languageCode) ||
      (track && track.languageCode) || '';
  };
  var translateable = function (track) {
    return track.is_translateable !== false && track.isTranslateable !== false;
  };
  var choose = function (authored, asr, fallback) {
    if (type === 'author_only') return authored;
    if (type === 'auto_only') return asr;
    if (type === 'auto_first') return asr || authored || fallback;
    return authored || asr || fallback;
  };
  var translationBase = function (authored, asr) {
    return type === 'author_first' ? authored || asr : asr || authored;
  };
  var copyTranslation = function (base, target) {
    var translated = {};
    for (var key in base)
      if (Object.prototype.hasOwnProperty.call(base, key)) translated[key] = base[key];
    translated.translationLanguage = {
      languageCode: target.languageCode,
      languageName: target.languageName
    };
    return translated;
  };
  var option = function (module, name, args) {
    try {
      return args ? pl.getOption(module, name, args) : pl.getOption(module, name);
    } catch (e) {
      return null;
    }
  };

  var mod = null, tracks = null;
  var modules = ['captions', 'cc'];
  for (var i = 0; i < modules.length; i++) {
    try {
      if (typeof pl.loadModule === 'function') pl.loadModule(modules[i]);
    } catch (e) {}
    var tracklist = option(modules[i], 'tracklist', { includeAsr: true });
    if (tracklist == null) tracklist = option(modules[i], 'tracklist');
    if (tracklist != null) {
      mod = modules[i];
      tracks = tracklist;
      break;
    }
  }
  if (mod === null) return p.op === 'apply' ? 'wait' : 'na';

  var transLangs = option(mod, 'translationLanguages') || [];
  var current = option(mod, 'track');
  var curOn = !!(current && (current.languageCode || current.vss_id || current.displayName));
  var curEff = curOn ? sub(effLang(current)) : '';
  var haveTracks = tracks.length > 0;

  var directTrack = function (want) {
    var authored = null, asr = null;
    for (var i = 0; i < tracks.length; i++) {
      var track = tracks[i];
      if (isTrans(track) || sub(track.languageCode) !== want) continue;
      if (isAsr(track)) {
        if (!asr) asr = track;
      } else if (!authored) {
        authored = track;
      }
    }
    return choose(authored, asr, null);
  };
  var translationTrack = function (want) {
    if (type === 'author_only') return null;
    var target = null;
    for (var i = 0; i < transLangs.length; i++)
      if (sub(transLangs[i].languageCode) === want) {
        target = transLangs[i];
        break;
      }
    if (!target) return null;
    var authored = null, asr = null;
    for (var j = 0; j < tracks.length; j++) {
      var track = tracks[j];
      if (isTrans(track) || !translateable(track)) continue;
      if (isAsr(track)) {
        if (!asr) asr = track;
      } else if (!authored) {
        authored = track;
      }
    }
    var base = translationBase(authored, asr);
    return base ? copyTranslation(base, target) : null;
  };
  var fallbackTrack = function () {
    var authored = null, asr = null, first = tracks[0] || null;
    for (var i = 0; i < tracks.length; i++) {
      var track = tracks[i];
      if (isTrans(track)) continue;
      if (isAsr(track)) {
        if (!asr) asr = track;
      } else if (!authored) {
        authored = track;
      }
    }
    return choose(authored, asr, first);
  };
  var pickTrack = function (langs, allowFallback) {
    for (var i = 0; i < langs.length; i++) {
      var want = sub(langs[i]);
      if (!want) continue;
      var track = directTrack(want) || translationTrack(want);
      if (track) return track;
    }
    return allowFallback ? fallbackTrack() : null;
  };
  var turnOff = function () {
    if (!curOn) return 'off';
    try {
      pl.setOption(mod, 'track', {});
    } catch (e) {}
    return 'off';
  };
  var applyTrack = function (track) {
    var lang = sub(effLang(track));
    if (curOn && lang === curEff) return 'on:' + lang;
    try {
      pl.setOption(mod, 'track', track);
    } catch (e) {
      return 'na';
    }
    return 'on:' + lang;
  };
  var turnOn = function (langs) {
    if (!haveTracks) return 'none';
    var track = pickTrack(langs, true);
    return track ? applyTrack(track) : 'none';
  };

  if (p.op === 'toggle') {
    if (curOn) return turnOff();
    return turnOn(p.mode === 'youtube' ? [p.sys || ''] : (p.langs || []));
  }

  var relax = !!p.relax;
  if (!p.known) {
    if (curOn) {
      var seed = pickTrack(p.langs || [], false);
      if (seed) return 'seed_' + applyTrack(seed);
      if (!relax) return 'pending';
      return 'seed_on:' + sub(effLang(current));
    }
    return haveTracks ? 'seed_off' : 'wait';
  }
  if (!p.on) return turnOff();
  if (!haveTracks) return 'wait';
  var preferred = pickTrack(p.langs || [], false);
  if (preferred) return applyTrack(preferred);
  if (!relax) {
    turnOff();
    return 'pending';
  }
  return turnOn(p.langs || []);
})
