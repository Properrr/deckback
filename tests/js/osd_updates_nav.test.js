// Node behavioural test for the Updates tab of config/scripts/osd.js, driven through a mock DOM:
// key presses must reach the whole release notes, which a source grep cannot show.
// Run: node tests/js/osd_updates_nav.test.js   (also wired into ctest when node is present).
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

// ---- minimal DOM -------------------------------------------------------------------------------
// Only what osd.js touches. Layout is faked: the notes box is a 300 px window onto 1200 px of text.
const NOTES_VIEWPORT = 300;
const NOTES_CONTENT = 1200;

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
  // Clip height and inner content height: what applyScroll() does its arithmetic on.
  Object.defineProperty(node, 'clientHeight', {
    get() { return node.classList.contains('scroll') ? NOTES_VIEWPORT : 0; },
  });
  Object.defineProperty(node, 'scrollHeight', {
    get() { return node.classList.contains('sinner') ? NOTES_CONTENT : 0; },
  });
  function walk(n, out, pred) {
    for (const c of n.children) { if (pred(c)) out.push(c); walk(c, out, pred); }
    return out;
  }
  // Supports only the selector forms osd.js uses: '.cls' and '[data-focus]'.
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
  // osd.js assigns node.id directly, so mirror it into the attribute map getElementById reads.
  return documentElement;
}

const PARAMS = {
  op: 'open',
  tab: 'settings',
  keys: [['A', 'Select']],
  cc: null,
  upd_has: true,
  upd_status: 'v0.0.8 is available. You have v0.0.6.',
  upd_notes: Array.from({ length: 40 }, (_, i) => 'changelog line ' + i).join('\n'),
  upd_buttons: [['update.confirm', 'Update now'], ['update.ignore', 'Ignore this version']],
  about_name: 'Deckback',
  about_version: '0.0.6',
  exit_enabled: true,
  hold_ms: 800,
};

function open(overrides) {
  installDom();
  const fn = eval(body);
  const rc = fn(Object.assign({}, PARAMS, overrides || {}));
  return { rc, S: global.window.__dbOSD };
}

function exec(S, cmd) { return S.exec(cmd); }

// ---- the drive ---------------------------------------------------------------------------------
{
  const { rc, S } = open();
  check(rc === 'ok', 'open returns ok');
  check(S && typeof S.exec === 'function', 'the component is published on window.__dbOSD');

  exec(S, 'tab_next');
  check(S.state().indexOf('tab=updates') === 0, 'L1/R1 reaches the Updates tab, got ' + S.state());

  const notes = S.notesBox();
  check(!!notes, 'the Updates tab has a notes box when notes were supplied');

  // ↑/↓ move the notes, not the focus ring.
  const before = notes.__off;
  const idxBefore = S.focusIdx;
  exec(S, 'down');
  check(notes.__off > before, 'down scrolls the release notes (was ' + before + ', now ' + notes.__off + ')');
  check(S.focusIdx === idxBefore, 'down does not move the focus ring away from the buttons');

  exec(S, 'up');
  check(notes.__off === before, 'up scrolls back');
  // ...and cannot be dragged above the top.
  exec(S, 'up');
  check(notes.__off === 0, 'scrolling stops at the top instead of going negative');

  // Enough presses reach the end of the changelog.
  for (let i = 0; i < 50; i++) exec(S, 'down');
  check(notes.__off === NOTES_CONTENT - NOTES_VIEWPORT,
    'the notes scroll all the way to the last line, got ' + notes.__off);

  // ←/→ carry focus between the two action buttons, which sit in one horizontal row.
  S.focusIdx = 0;
  exec(S, 'right');
  check(S.focusIdx === 1, 'right moves to the second action button');
  exec(S, 'left');
  check(S.focusIdx === 0, 'left moves back to the first');
  exec(S, 'left');
  check(S.focusIdx === 0, 'left clamps at the first button');

  // A is still what activates, and it must report the focused button's action.
  check(exec(S, 'select') === 'action:update.confirm', 'A activates the focused button');
}

// The hint bar advertises the rebound keys.
{
  const { S } = open();
  exec(S, 'tab_next');
  check(S.navHint.textContent.indexOf('Scroll notes') !== -1,
    'the Updates tab hint says ↑↓ scrolls, got: ' + S.navHint.textContent);
  exec(S, 'tab_prev');
  check(S.navHint.textContent.indexOf('Scroll notes') === -1,
    'the Settings tab keeps the default hint, got: ' + S.navHint.textContent);
}

// With no notes, ↑/↓ fall back to moving focus so the buttons stay reachable.
{
  const { S } = open({ upd_notes: '' });
  exec(S, 'tab_next');
  check(S.notesBox() === null, 'no notes box when there are no notes');
  S.focusIdx = 0;
  exec(S, 'down');
  check(S.focusIdx === 1, 'without notes, down moves the focus ring again');
  check(S.navHint.textContent.indexOf('Scroll notes') === -1, 'and the hint does not promise scrolling');
}

// The check-only shape: one button, no update announced.
{
  const { S } = open({ upd_has: false, upd_buttons: [['update.check', 'Check for updates']] });
  exec(S, 'tab_next');
  check(exec(S, 'select') === 'action:update.check', 'A runs the manual check');
}

// A verdict arriving while the menu is open patches the panel in place.
{
  const { S } = open({ upd_has: false, upd_buttons: [['update.check', 'Check for updates']] });
  exec(S, 'tab_next');
  const status = () => S.panels.updates.querySelector('.status').textContent;
  const labels = () =>
    S.panels.updates.querySelector('.actions').children.map((c) => c.getAttribute('data-action'));

  // In flight: the buttons go away, because pressing anything again would be ignored anyway.
  let rc = S.setUpdate({ upd_has: false, upd_status: 'Updating… this can take a minute.', upd_buttons: [] });
  check(rc === 'ok', 'setUpdate patches an open menu');
  check(status() === 'Updating… this can take a minute.', 'the status line updates in place');
  check(labels().length === 0, 'the buttons clear while a deploy is in flight');
  check(S.focusIdx === -1, 'focus is released when nothing is focusable');

  // The verdict: the user sees the answer without reopening anything.
  rc = S.setUpdate({
    upd_has: false,
    upd_status: 'You are on the latest version.',
    upd_buttons: [['update.check', 'Check for updates']],
  });
  check(rc === 'ok', 'setUpdate patches again');
  check(status() === 'You are on the latest version.', 'the verdict lands in the open panel');
  check(labels().join() === 'update.check', 'the check button comes back');
  check(S.focusIdx === 0, 'focus is restored onto the rebuilt button');
  check(exec(S, 'select') === 'action:update.check', 'and the rebuilt button is still live');

  // An update appearing mid-session swaps in the deploy pair and lights the tab badge.
  S.setUpdate({
    upd_has: true,
    upd_status: 'v0.0.9 is available. You have v0.0.8.',
    upd_buttons: [['update.confirm', 'Update now'], ['update.ignore', 'Ignore this version']],
  });
  check(labels().join() === 'update.confirm,update.ignore', 'the deploy pair replaces the check button');
  check(S.tabs.updates.classList.contains('badge'), 'the Updates tab badges when an update appears');
}

// A patch aimed at a menu that is gone answers 'gone' (the launcher then clears its captured
// state) instead of throwing.
{
  const { S } = open();
  S.root.remove();
  const fn = eval(body);
  check(fn({ op: 'upd', upd_status: 'x' }) === 'gone', 'a patch on a torn-down menu reports gone');
  void S;
}

if (failures) {
  console.error(failures + ' failure(s)');
  process.exit(1);
}
console.log('osd_updates_nav.test.js: ok');
