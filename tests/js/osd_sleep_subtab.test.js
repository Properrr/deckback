// Node behavioural test for the Sleep sub-tab of config/scripts/osd.js, driven through a mock DOM.
//
// The change this covers is a generalisation: the combo widget used to emit "apply:cc.<key>=<val>"
// with the namespace hard-coded, and now takes it from the model's own `ns`. That is a silent-failure
// shape — a Sleep edit that still said `cc.` would be routed to CaptionSettings, which would reject
// it and return false, and the launcher would drop the setting with no error anywhere. Nothing on
// screen would look wrong: the combo would show the new value, because the page updates its own text
// before the verdict is ever read. So both directions are pinned here, on one open menu.
//
// The countdown patch (op:"sleep") is covered for the same reason: it runs once a second while the
// menu is open, and a patch that re-rendered instead of updating in place would reset the focus ring
// under the user's thumb once a second — unusable, and invisible to any source grep.
//
// Run: node tests/js/osd_sleep_subtab.test.js   (also wired into ctest when node is present).
'use strict';
const fs = require('fs');
const path = require('path');

const SCRIPT = path.join(__dirname, '..', '..', 'config', 'scripts', 'osd.js');
const body = fs.readFileSync(SCRIPT, 'utf8').trim();

let failures = 0;
function check(cond, msg) {
  if (!cond) {
    failures++;
    console.error('FAIL: ' + msg);
  }
}

// ---- minimal DOM (only what osd.js touches) ------------------------------------------------------
function makeEl(tag) {
  const node = {
    tagName: String(tag).toUpperCase(),
    className: '',
    children: [],
    parentNode: null,
    isConnected: false,
    textContent: '',
    attrs: {},
    offsetTop: 0,
    offsetHeight: 20,
    offsetWidth: 100,
    style: { props: {}, setProperty(k, v) { this.props[k] = v; }, getPropertyValue(k) { return this.props[k]; } },
    setAttribute(k, v) { this.attrs[k] = String(v); },
    getAttribute(k) { return Object.prototype.hasOwnProperty.call(this.attrs, k) ? this.attrs[k] : null; },
    appendChild(c) { c.parentNode = node; c.isConnected = true; node.children.push(c); return c; },
    removeChild(c) { const i = node.children.indexOf(c); if (i >= 0) node.children.splice(i, 1); c.isConnected = false; },
    remove() { if (node.parentNode) node.parentNode.removeChild(node); },
    scrollIntoView() {},
  };
  node.classList = {
    add(c) { if (!node.className.split(' ').includes(c)) node.className = (node.className + ' ' + c).trim(); },
    remove(c) { node.className = node.className.split(' ').filter((x) => x && x !== c).join(' '); },
    contains(c) { return node.className.split(' ').includes(c); },
    toggle(c, on) { if (on) node.classList.add(c); else node.classList.remove(c); },
  };
  // renderSub() empties a container with `while (n.firstChild) n.removeChild(n.firstChild)`, so a
  // stub without firstChild silently keeps every sub-tab's rows stacked on top of each other — and
  // the focus ring then lands on the previous sub-tab's widget. Model it.
  Object.defineProperty(node, 'firstChild', {
    get() { return node.children.length ? node.children[0] : null; },
  });
  Object.defineProperty(node, 'clientHeight', {
    get() { return node.classList.contains('scroll') ? 300 : 0; },
  });
  Object.defineProperty(node, 'scrollHeight', {
    get() { return node.classList.contains('sinner') ? 1200 : 0; },
  });
  function walk(n, out, pred) {
    for (const c of n.children) { if (pred(c)) out.push(c); walk(c, out, pred); }
    return out;
  }
  function matches(n, sel) {
    if (sel.startsWith('.')) return n.classList.contains(sel.slice(1));
    if (sel.startsWith('[') && sel.endsWith(']')) {
      const inner = sel.slice(1, -1);
      const eq = inner.indexOf('=');
      if (eq === -1) return n.getAttribute(inner) !== null;
      return n.getAttribute(inner.slice(0, eq)) === inner.slice(eq + 1).replace(/^["']|["']$/g, '');
    }
    throw new Error('unsupported selector in stub: ' + sel);
  }
  node.querySelectorAll = (sel) => walk(node, [], (n) => matches(n, sel));
  node.querySelector = (sel) => node.querySelectorAll(sel)[0] || null;
  return node;
}

function installDom() {
  const documentElement = makeEl('html');
  documentElement.isConnected = true;
  global.document = {
    documentElement,
    adoptedStyleSheets: [],
    createElement: makeEl,
    getElementById(id) {
      const hit = documentElement.querySelectorAll('[id]').filter((n) => n.id === id);
      return hit[0] || null;
    },
  };
  global.window = {
    CSSStyleSheet: function () { this.replaceSync = () => {}; },
    MutationObserver: function (cb) { this.observe = () => {}; this.disconnect = () => {}; void cb; },
  };
  global.CSSStyleSheet = global.window.CSSStyleSheet;
  return documentElement;
}

// The models the launcher actually sends: SleepTimer::osd_model_json and
// CaptionSettings::osd_model_json, reduced to the fields the page reads.
const SLEEP_MODEL = {
  ns: 'sleep',
  status: 'Off',
  note: 'When the timer runs out playback pauses.',
  rows: [{
    key: 'timer',
    label: 'Stop playback in',
    kind: 'combo',
    value: '0',
    options: [
      { value: '0', label: 'Off' },
      { value: '5', label: '5 min' },
      { value: '15', label: '15 min' },
    ],
  }],
};

const CC_MODEL = {
  ns: 'cc',
  rows: [{
    key: 'type',
    label: 'Caption type',
    kind: 'combo',
    value: 'author_first',
    options: [
      { value: 'author_first', label: 'Author, then auto' },
      { value: 'author_only', label: 'Author only' },
    ],
  }],
  langs: [{ value: 'en', label: 'English' }],
};

const PARAMS = {
  op: 'open',
  tab: 'settings',
  keys: [['A', 'Select']],
  cc: CC_MODEL,
  sleep: SLEEP_MODEL,
  upd_has: false,
  upd_status: 'Update status is not available.',
  upd_notes: '',
  upd_buttons: [],
  about_name: 'Deckback',
  about_version: '0.0.9',
  exit_enabled: true,
  hold_ms: 800,
};

function open(overrides) {
  installDom();
  const fn = eval(body);
  // A fresh deep copy per open: the component MUTATES the model's row values in place as the user
  // cycles, so a shared literal would leak state between cases and hide an ordering bug.
  const params = Object.assign({}, PARAMS, overrides || {});
  params.cc = params.cc ? JSON.parse(JSON.stringify(params.cc)) : params.cc;
  params.sleep = params.sleep ? JSON.parse(JSON.stringify(params.sleep)) : params.sleep;
  const rc = fn(params);
  return { rc, S: global.window.__dbOSD };
}

function exec(S, cmd) { return S.exec(cmd); }

// Walk the Section row until the named sub-tab is showing. Focus starts on `subsel`, and ←/→ there
// cycles — the same path a thumb takes.
function gotoSub(S, name) {
  for (let i = 0; i < 6 && S.sub !== name; i++) exec(S, 'right');
  return S.sub === name;
}

// ---- the sub-tab exists and is reachable ---------------------------------------------------------
{
  const { rc, S } = open();
  check(rc === 'ok', 'open returns ok');
  check(S.subs.join(',') === 'keys,captions,sleep',
    'the Section row offers Keys, Captions and Sleep, got: ' + S.subs.join(','));

  check(gotoSub(S, 'sleep'), 'the Section row reaches the Sleep sub-tab, stuck at ' + S.sub);
  check(S.subLabel.textContent === 'Sleep', 'the Section row is labelled Sleep, got ' + S.subLabel.textContent);

  const stat = S.subContent.querySelector('.sstat');
  check(!!stat && stat.textContent === 'Off',
    'the launcher-computed status line renders, got ' + (stat && stat.textContent));
  check(!!S.subContent.querySelector('.snote'), 'the explanatory note renders');
  const row = S.subContent.querySelector('.crow');
  check(!!row && row.getAttribute('data-key') === 'timer', 'the duration combo renders');
}

// ---- THE regression: a Sleep edit must name its own namespace ------------------------------------
{
  const { S } = open();
  gotoSub(S, 'sleep');
  exec(S, 'down');  // off the Section row, onto the duration combo
  const verdict = exec(S, 'right');
  check(verdict === 'apply:sleep.timer=5',
    'cycling the duration emits a sleep-namespaced verdict, got ' + verdict);
  check(exec(S, 'right') === 'apply:sleep.timer=15', 'a second cycle advances the ladder');
  // Wrapping past the end returns to Off, which is how a user cancels without leaving the row.
  check(exec(S, 'right') === 'apply:sleep.timer=0', 'the ladder wraps back to Off');
  check(exec(S, 'left') === 'apply:sleep.timer=15', 'left walks the ladder backwards');

  // And the value the user sees must be the value the verdict carried, or the menu is lying about
  // what the launcher was told.
  const vtext = S.subContent.querySelector('.cval').children.filter((c) => !c.classList.contains('arrow'));
  check(vtext.length === 1 && vtext[0].textContent === '15 min',
    'the combo shows the label of the value it just sent, got ' + (vtext[0] && vtext[0].textContent));
}

// ---- ...and Captions must be unaffected by that generalisation -----------------------------------
{
  const { S } = open();
  check(gotoSub(S, 'captions'), 'the Section row still reaches Captions');
  exec(S, 'down');
  const verdict = exec(S, 'right');
  check(verdict === 'apply:cc.type=author_only',
    'a caption edit still emits a cc-namespaced verdict, got ' + verdict);
}

// A model with no `ns` (an older launcher, or the hand-built model tests/deck/test_osd.py injects)
// must keep behaving as captions rather than emitting a namespaceless verdict.
{
  const cc = JSON.parse(JSON.stringify(CC_MODEL));
  delete cc.ns;
  const { S } = open({ cc, sleep: null });
  gotoSub(S, 'captions');
  exec(S, 'down');
  check(exec(S, 'right') === 'apply:cc.type=author_only',
    'a model without ns falls back to the cc namespace');
}

// ---- the countdown patch -------------------------------------------------------------------------
{
  const { S } = open();
  gotoSub(S, 'sleep');
  exec(S, 'down');
  const focusBefore = S.focusIdx;

  S.setSleep({
    ns: 'sleep',
    status: 'Stops in 5 min',
    rows: [Object.assign({}, SLEEP_MODEL.rows[0], { value: '5' })],
  });
  check(S.subContent.querySelector('.sstat').textContent === 'Stops in 5 min',
    'the patch updates the countdown text');
  check(S.focusIdx === focusBefore,
    'the patch does not move the focus ring (it runs once a second while the menu is open)');
  const vtext = S.subContent.querySelector('.cval').children.filter((c) => !c.classList.contains('arrow'));
  check(vtext[0].textContent === '5 min', 'the patch updates the combo value in place');

  // A patch aimed at a sub-tab that is not showing must be stored, not drawn — the user could be on
  // Keys while the timer runs.
  gotoSub(S, 'keys');
  check(S.setSleep({ ns: 'sleep', status: 'Stops in 4 min', rows: [] }) === 'ok',
    'a patch while another sub-tab is showing is accepted');
  check(S.sleep.status === 'Stops in 4 min', 'and is remembered for the next render');
}

// ---- the feature turned off ----------------------------------------------------------------------
{
  const { S } = open({ sleep: null });
  check(S.subs.indexOf('sleep') === -1,
    'with sleep_timer off the sub-tab is not offered at all, got: ' + S.subs.join(','));
  check(gotoSub(S, 'captions'), 'and the remaining sub-tabs still cycle');
}

if (failures) {
  console.error(failures + ' check(s) failed');
  process.exit(1);
}
console.log('osd_sleep_subtab: all checks passed');
