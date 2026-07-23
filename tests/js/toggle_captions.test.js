// Node fuzz/combinatorial test for config/scripts/toggle_captions.js — the caption pick/toggle/apply
// logic, exercised against a MOCK YouTube player so every op/mode/state/track scenario is covered
// off-device (the launcher's C++ side is covered by caption_fuzz_test.cpp; this covers the JS the
// launcher only evaluates on the Deck).
//
// Run: node tests/js/toggle_captions.test.js   (also wired into ctest when node is present).
'use strict';
const fs = require('fs');
const path = require('path');

const SCRIPT = path.join(__dirname, '..', '..', 'config', 'scripts', 'toggle_captions.js');
const body = fs.readFileSync(SCRIPT, 'utf8').trim();

let failures = 0;
function check(cond, msg) {
  if (!cond) {
    failures++;
    console.error('FAIL: ' + msg);
  }
}

function mkPlayer(tracks, active, liveModule, transLangs) {
  liveModule = liveModule || 'captions';
  transLangs = transLangs || [];
  let cur = active || {};
  return {
    loadModule() {},
    getOption(mod, opt) {
      if (mod !== liveModule) return null;
      if (opt === 'tracklist') return tracks;
      if (opt === 'translationLanguages') return transLangs;
      if (opt === 'track') return cur;
      return null;
    },
    setOption(mod, opt, val) {
      if (mod === liveModule && opt === 'track') cur = val || {};
    },
    active() {
      return cur;
    },
  };
}

function run(params, player, hash) {
  const location = { hash: hash === undefined ? '#/watch?v=abc' : hash };
  const document = {
    querySelector(sel) {
      return sel.indexOf('html5-video-player') >= 0 ? player : null;
    },
  };
  const fn = eval(body);
  const result = fn(params);
  const a = player && player.active ? player.active() : {};
  const effLang = a && a.translationLanguage && a.translationLanguage.languageCode
    ? a.translationLanguage.languageCode
    : a && a.languageCode ? a.languageCode : null;
  return { result, activeLang: a && a.languageCode ? a.languageCode : null, effLang, active: a };
}

const authored = (lc) => ({ languageCode: lc });
const asr = (lc) => ({ languageCode: lc, kind: 'asr' });
const trans = (lc) => ({ languageCode: lc, translationLanguage: { languageCode: lc } });
const target = (lc, name) => ({ languageCode: lc, languageName: name || lc.toUpperCase() });

// ---- explicit correctness scenarios --------------------------------------------------------------

function scenarios() {
  {
    const p = mkPlayer([authored('en')], {});
    const r = run({ op: 'toggle', mode: 'local', langs: ['en'], type: 'author_first' }, p, '#/');
    check(r.result === 'na', 'off-watch toggle returns na, got ' + r.result);
    check(r.activeLang === null, 'off-watch leaves captions off');
  }
  {
    const p = mkPlayer([authored('en'), authored('uk')], {});
    const r = run({ op: 'toggle', mode: 'local', langs: ['uk', 'en'], type: 'author_first' }, p);
    check(r.result === 'on:uk', 'preferred [uk,en] picks uk, got ' + r.result);
    check(r.activeLang === 'uk', 'active track is uk');
  }
  {
    const p = mkPlayer([authored('en'), authored('uk')], authored('uk'));
    const r = run({ op: 'toggle', mode: 'local', langs: ['en'], type: 'author_first' }, p);
    check(r.result === 'off', 'toggle when ON turns off, got ' + r.result);
    check(r.activeLang === null, 'captions cleared');
  }
  {
    const p = mkPlayer([authored('en'), authored('de')], {});
    const r = run({ op: 'toggle', mode: 'youtube', langs: ['de'], type: 'author_first', sys: 'en' }, p);
    check(r.result === 'on:en', 'youtube mode uses sys=en, got ' + r.result);
  }
  {
    const p = mkPlayer([], {});
    const r = run({ op: 'toggle', mode: 'local', langs: ['en'], type: 'author_first' }, p);
    check(r.result === 'none', 'no tracks -> none, got ' + r.result);
  }
  {
    const tracks = [authored('en'), asr('en')];
    let r = run({ op: 'toggle', mode: 'local', langs: ['en'], type: 'author_first' },
                mkPlayer(tracks, {}));
    check(r.result === 'on:en' && r.activeLang === 'en', 'author_first picks en authored');
    r = run({ op: 'toggle', mode: 'local', langs: ['en'], type: 'auto_first' }, mkPlayer(tracks, {}));
    check(r.result === 'on:en', 'auto_first still en (same lang), got ' + r.result);
  }
  {
    const p = mkPlayer([asr('en')], {});
    const r = run({ op: 'toggle', mode: 'local', langs: ['en'], type: 'author_only' }, p);
    check(r.result === 'none', 'author_only with only asr -> none, got ' + r.result);
  }
  {
    const p = mkPlayer([authored('en')], {}, 'none');
    const r = run({ op: 'apply', mode: 'local', on: true, known: true, langs: ['en'],
                    type: 'author_first' }, p);
    check(r.result === 'wait', 'apply before module loads -> wait, got ' + r.result);
  }
  {
    const p = mkPlayer([authored('en'), authored('uk')], authored('uk'));
    const r = run({ op: 'apply', mode: 'local', on: true, known: true, langs: ['en'],
                    type: 'author_first' }, p);
    check(r.result === 'on:en' && r.activeLang === 'en', 'apply on enforces en, got ' + r.result);
  }
  {
    const p = mkPlayer([authored('uk')], authored('uk'));
    const r = run({ op: 'apply', mode: 'local', on: false, known: true, langs: ['en'],
                    type: 'author_first' }, p);
    check(r.result === 'off' && r.activeLang === null, 'apply off clears, got ' + r.result);
  }
  {
    const p = mkPlayer([authored('en'), authored('uk')], authored('uk'));
    const r = run({ op: 'apply', mode: 'local', on: false, known: false, langs: ['en'],
                    type: 'author_first' }, p);
    check(r.result === 'seed_on:en' && r.activeLang === 'en', 'seed from YT-on, got ' + r.result);
  }
  {
    const p = mkPlayer([authored('en')], {});
    const r = run({ op: 'apply', mode: 'local', on: false, known: false, langs: ['en'],
                    type: 'author_first' }, p);
    check(r.result === 'seed_off', 'seed from YT-off -> seed_off, got ' + r.result);
  }
  {
    const p = mkPlayer([], {});
    const r = run({ op: 'apply', mode: 'local', on: false, known: false, langs: ['en'],
                    type: 'author_first' }, p);
    check(r.result === 'wait', 'seed with no tracks yet -> wait, got ' + r.result);
  }
  {
    const p = mkPlayer([authored('en')], {}, 'cc');
    const r = run({ op: 'toggle', mode: 'local', langs: ['en'], type: 'author_first' }, p);
    check(r.result === 'on:en', 'legacy cc module works, got ' + r.result);
  }
  {
    let r = run({ op: 'apply', mode: 'local', on: false, known: false, langs: ['en'],
                  type: 'author_only' }, mkPlayer([asr('uk')], asr('uk')));
    check(r.result === 'pending', 'strict seed holds as pending, got ' + r.result);
    check(r.activeLang === 'uk', 'YouTube track left on while pending');
    r = run({ op: 'apply', mode: 'local', on: false, known: false, relax: true, langs: ['en'],
              type: 'author_only' }, mkPlayer([asr('uk')], asr('uk')));
    check(r.result === 'seed_on:uk', 'relaxed seed adopts YT track, got ' + r.result);
    check(r.activeLang === 'uk', 'YouTube track left on');
  }
  {
    const tracks = [authored('ru'), asr('ru')];
    const tl = [target('en', 'English'), target('de', 'German')];
    const r = run({ op: 'toggle', mode: 'local', langs: ['en'], type: 'auto_only' },
                  mkPlayer(tracks, {}, 'captions', tl));
    check(r.result === 'on:en', 'auto_only [en] picks the en translation, got ' + r.result);
    check(r.effLang === 'en', 'displayed language is en, got ' + r.effLang);
    check(r.active.translationLanguage && r.active.translationLanguage.languageCode === 'en',
          'applied track carries translationLanguage=en');
    check(r.active.languageCode === 'ru', 'translation base is a ru track');
  }
  {
    const r = run({ op: 'toggle', mode: 'local', langs: ['en'], type: 'auto_only' },
                  mkPlayer([authored('ru')], {}, 'captions', [target('en', 'English')]));
    check(r.result === 'on:en' && r.active.translationLanguage,
          'auto_only falls back to an authored translation base, got ' + r.result);
  }
  {
    const tracks = [authored('ru')];
    const tl = [target('en', 'English')];
    const r = run({ op: 'apply', mode: 'local', on: true, known: true, langs: ['en'],
                    type: 'author_first' }, mkPlayer(tracks, authored('ru'), 'captions', tl));
    check(r.result === 'on:en' && r.effLang === 'en', 'apply enforces en via translation, got ' + r.result);
  }
  {
    const tracks = [authored('ru')];
    const tl = [target('en', 'English')];
    const r = run({ op: 'toggle', mode: 'local', langs: ['en'], type: 'author_only' },
                  mkPlayer(tracks, {}, 'captions', tl));
    check(r.result === 'on:ru' && r.effLang === 'ru', 'author_only does not translate, got ' + r.result);
    check(!r.active.translationLanguage, 'no translation applied under author_only');
  }
  {
    const tracks = [authored('ru'), authored('en')];
    const tl = [target('en', 'English')];
    const r = run({ op: 'toggle', mode: 'local', langs: ['en'], type: 'author_first' },
                  mkPlayer(tracks, {}, 'captions', tl));
    check(r.result === 'on:en' && !r.active.translationLanguage,
          'direct en track beats translation, got ' + r.result);
  }
  {
    const tracks = [{ languageCode: 'ru', is_translateable: false }];
    const tl = [target('en', 'English')];
    const r = run({ op: 'toggle', mode: 'local', langs: ['en'], type: 'auto_first' },
                  mkPlayer(tracks, {}, 'captions', tl));
    check(!r.active.translationLanguage, 'no translation off a non-translateable base');
  }
  {
    const tracks = [authored('ru'), asr('ru')];
    const tl = [target('en', 'English')];
    const r = run({ op: 'apply', mode: 'local', on: false, known: false, langs: ['en'],
                    type: 'auto_only' }, mkPlayer(tracks, asr('ru'), 'captions', tl));
    check(r.result === 'seed_on:en' && r.effLang === 'en', 'seed switches ru->en translation, got ' + r.result);
  }
  {
    const tracks = [authored('ru')];
    let r = run({ op: 'toggle', mode: 'local', langs: ['uk', 'en'], type: 'author_first' },
                mkPlayer(tracks, {}, 'captions', [target('uk', 'Ukrainian'), target('en', 'English')]));
    check(r.effLang === 'uk', 'first preferred uk translation wins, got ' + r.effLang);
    r = run({ op: 'toggle', mode: 'local', langs: ['uk', 'en'], type: 'author_first' },
            mkPlayer(tracks, {}, 'captions', [target('en', 'English')]));
    check(r.effLang === 'en', 'uk not offered -> en translation, got ' + r.effLang);
  }
  {
    const tracks = [authored('ru'), asr('ru')];
    const r = run({ op: 'apply', mode: 'local', on: true, known: true, langs: ['en'],
                    type: 'author_first' }, mkPlayer(tracks, authored('ru'), 'captions', []));
    check(r.result === 'pending', 'strict apply holds for en, got ' + r.result);
    check(r.activeLang === null, 'wrong-language track cleared while pending');
  }
  {
    const tracks = [authored('ru'), asr('ru')];
    const r = run({ op: 'apply', mode: 'local', on: true, known: true, langs: ['en'],
                    type: 'author_first' },
                  mkPlayer(tracks, authored('ru'), 'captions', [target('en', 'English')]));
    check(r.result === 'on:en' && r.effLang === 'en', 'apply upgrades to en once loaded, got ' + r.result);
  }
  {
    const tracks = [authored('ru'), asr('ru')];
    const r = run({ op: 'apply', mode: 'local', on: true, known: true, relax: true, langs: ['en'],
                    type: 'author_first' }, mkPlayer(tracks, {}, 'captions', []));
    check(r.result.indexOf('on:') === 0 && r.result !== 'on:en',
          'relaxed apply falls back to an available track, got ' + r.result);
  }
  {
    const r = run({ op: 'apply', mode: 'local', on: false, known: true, langs: ['en'],
                    type: 'author_first' }, mkPlayer([authored('ru')], authored('ru')));
    check(r.result === 'off' && r.activeLang === null, 'apply off clears YT default, got ' + r.result);
  }
  {
    const tl = [target('en', 'English')];
    const p = mkPlayer([authored('ru')],
                       { languageCode: 'ru', translationLanguage: { languageCode: 'en' } }, 'captions', tl);
    let sets = 0;
    const origSet = p.setOption;
    p.setOption = function (m, o, v) { sets++; return origSet.call(p, m, o, v); };
    const r = run({ op: 'apply', mode: 'local', on: true, known: true, langs: ['en'],
                    type: 'author_first' }, p);
    check(r.result === 'on:en', 'idempotent apply stays on:en, got ' + r.result);
    check(sets === 0, 'no redundant setOption when already showing en, got ' + sets);
  }
}

function sweep() {
  const ops = ['toggle', 'apply'];
  const modes = ['local', 'youtube'];
  const types = ['author_first', 'auto_first', 'author_only', 'auto_only'];
  const langSets = [['en'], ['uk', 'en'], ['fr'], ['system'], []];
  const trackSets = [
    [],
    [authored('en')],
    [authored('en'), authored('uk')],
    [authored('en'), asr('en')],
    [authored('en'), authored('uk'), asr('fr'), trans('de')],
    [authored('ru'), asr('ru')],
  ];
  const actives = [{}, authored('en'), authored('uk')];
  const transSets = [[], [target('en', 'English'), target('de', 'German'), target('uk', 'Ukrainian')]];
  const valid = new Set(['off', 'none', 'na', 'wait', 'pending']);
  const primary = (lc) => String(lc).toLowerCase().split(/[-_]/)[0];
  let runs = 0;

  for (const op of ops)
    for (const mode of modes)
      for (const on of [true, false])
        for (const known of [true, false])
          for (const relax of [false, true])
            for (const type of types)
              for (const langs of langSets)
                for (const tracks of trackSets)
                  for (const transLangs of transSets)
                    for (const active of actives) {
                    const p = mkPlayer(tracks, active, 'captions', transLangs);
                    const params = { op, mode, on, known, relax, langs, type, sys: 'en' };
                    let r;
                    try {
                      r = run(params, p);
                    } catch (e) {
                      check(false, 'threw for ' + JSON.stringify(params) + ': ' + e);
                      continue;
                    }
                    runs++;
                    const res = r.result;
                    const ok = valid.has(res) || res.indexOf('on:') === 0 ||
                               res.indexOf('seed_on:') === 0 || res === 'seed_off';
                    check(ok, 'invalid result token: ' + res + ' for ' + JSON.stringify(params));
                    if (res.indexOf('on:') === 0) {
                      check(r.activeLang !== null, 'on:* but no active track (' + JSON.stringify(params) + ')');
                      check(res === 'on:' + primary(r.effLang),
                            'on token ' + res + ' != applied effLang ' + r.effLang);
                    }
                    if (res === 'off')
                      check(r.activeLang === null, 'off but a track is active');
                    if (res.indexOf('seed_') === 0)
                      check(known === false, 'seed result with known=true');
                    if (r.active && r.active.translationLanguage) {
                      const tgt = primary(r.active.translationLanguage.languageCode);
                      check(transLangs.some((t) => primary(t.languageCode) === tgt),
                            'translated to a language not offered: ' + tgt);
                    }
                  }
  return runs;
}

scenarios();
const runs = sweep();
if (failures) {
  console.error('toggle_captions.test.js: ' + failures + ' FAILURES');
  process.exit(1);
}
console.log('toggle_captions.test.js: all passed (' + runs + ' sweep runs + explicit scenarios)');
