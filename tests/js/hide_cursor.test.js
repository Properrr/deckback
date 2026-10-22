// Node test for config/scripts/hide_cursor.js: it hides the cursor and does nothing else.
// Run: node tests/js/hide_cursor.test.js   (also wired into ctest when node is present).
'use strict';
const fs = require('fs');
const path = require('path');

const body = fs.readFileSync(
  path.join(__dirname, '..', '..', 'config', 'scripts', 'hide_cursor.js'), 'utf8');

let failures = 0;
function check(cond, msg) {
  if (!cond) { failures++; console.error('FAIL: ' + msg); }
}

function mkWindow(opts) {
  opts = opts || {};
  const listeners = [];
  const win = {
    CSSStyleSheet: opts.noConstructable ? undefined
      : function () { this.replaceSync = function () {}; },
    document: {
      addEventListener(type) { listeners.push(type); },
      adoptedStyleSheets: [],
      documentElement: {style: {_p: {}, setProperty(k, v) { this._p[k] = v; }}},
      styleSheets: opts.sheets || []
    }
  };
  win._listeners = listeners;
  return win;
}

function load(win) {
  new Function('window', body)(win);
  return win;
}

// ---- tests ----

function testItHidesTheCursor() {
  const w = load(mkWindow());
  check(w.document.documentElement.style._p.cursor === 'none',
    'an inline baseline is set on documentElement');
  check(w.document.adoptedStyleSheets.length === 1,
    'and a constructable stylesheet is adopted (the CSP drops inline <style>)');
}

function testItFallsBackWithoutConstructableSheets() {
  const inserted = [];
  const w = mkWindow({
    noConstructable: true,
    sheets: [{cssRules: [], insertRule(r) { inserted.push(r); }}]
  });
  load(w);
  check(w.document.documentElement.style._p.cursor === 'none', 'the inline baseline still lands');
  check(inserted.length === 1 && /cursor:none/.test(inserted[0]),
    'and it falls back to insertRule on an existing sheet');
}

function testItReappliesOnNavigation() {
  const w = load(mkWindow());
  check(w._listeners.indexOf('DOMContentLoaded') >= 0,
    'Leanback replaces documentElement on navigation, so it must re-apply');
}

function testItTouchesNothingElse() {
  // It must not affect input: no event listener other than DOMContentLoaded.
  const w = load(mkWindow());
  check(w._listeners.length === 1 && w._listeners[0] === 'DOMContentLoaded',
    `it registers exactly one listener, got ${JSON.stringify(w._listeners)}`);
  check(!/addEventListener\s*\(\s*['"](touch|pointer|mouse|click)/.test(body),
    'and the source registers no input listeners at all');
}

function testItSurvivesAHostileDocument() {
  // A page that throws from every seam must not take the script down.
  const w = {
    CSSStyleSheet: function () { throw new Error('nope'); },
    document: {
      addEventListener() { throw new Error('nope'); },
      get adoptedStyleSheets() { throw new Error('nope'); },
      documentElement: {style: {setProperty() { throw new Error('nope'); }}},
      get styleSheets() { throw new Error('nope'); }
    }
  };
  let threw = null;
  try { load(w); } catch (e) { threw = e; }
  check(threw === null, `it must not throw out of a hostile document, got ${threw}`);
}

const tests = [
  testItHidesTheCursor,
  testItFallsBackWithoutConstructableSheets,
  testItReappliesOnNavigation,
  testItTouchesNothingElse,
  testItSurvivesAHostileDocument,
];
for (const t of tests) {
  try { t(); } catch (e) { failures++; console.error(`FAIL: ${t.name} threw ${e && e.stack}`); }
}
if (failures) {
  console.error(`hide_cursor.test.js: ${failures} failure(s)`);
  process.exit(1);
}
console.log(`hide_cursor.test.js: all ${tests.length} cases passed`);
