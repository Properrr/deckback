// Node test for config/scripts/touch_gestures.js — the gesture recogniser, driven by synthetic
// touch streams so every gesture, boundary and rejection is covered off-device.
//
// The recogniser is the one part of P12.4 that can be wrong in a way hardware will not reveal
// quickly: a threshold that makes a tap read as a drag, or an edge swipe as a scroll, produces "the
// touchscreen feels wrong" rather than a failure. Coordinates and timestamps are fed directly
// through the `_feed` seam, so this needs no DOM and no Deck.
//
// Run: node tests/js/touch_gestures.test.js   (also wired into ctest when node is present).
'use strict';
const fs = require('fs');
const path = require('path');

const SCRIPT = path.join(__dirname, '..', '..', 'config', 'scripts', 'touch_gestures.js');
const body = fs.readFileSync(SCRIPT, 'utf8');

let failures = 0;
function check(cond, msg) {
  if (!cond) {
    failures++;
    console.error('FAIL: ' + msg);
  }
}
function eq(got, want, msg) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  check(g === w, `${msg}\n     got:  ${g}\n     want: ${w}`);
}

// ---- a window just real enough for the script ----

function mkWindow() {
  const timers = new Map();
  let nextId = 1;
  const win = {
    innerWidth: 1280,
    innerHeight: 800,
    setTimeout(fn, ms) { const id = nextId++; timers.set(id, {fn, ms}); return id; },
    clearTimeout(id) { timers.delete(id); },
    addEventListener() {},
    removeEventListener() {},
    // Test control: run every pending timer whose delay is <= ms, in insertion order.
    _fire(ms) {
      for (const [id, t] of [...timers.entries()]) {
        if (t.ms <= ms) { timers.delete(id); t.fn(); }
      }
    },
    _pending() { return timers.size; }
  };
  win.document = {addEventListener() {}, removeEventListener() {}};
  return win;
}

function load() {
  const win = mkWindow();
  // The script is a self-invoking (function(){...})(); it reaches the page through `window`.
  new Function('window', body)(win);
  return {win, g: win.__deckbackGestures};
}

// ---- synthetic touch streams ----

let clock = 1000;
function evt(type, x, y, t, nTouches) {
  const touches = [];
  const n = nTouches === undefined ? (type === 'touchend' ? 0 : 1) : nTouches;
  for (let i = 0; i < n; i++) touches.push({clientX: x, clientY: y, identifier: i});
  return {
    type,
    timeStamp: t,
    cancelable: true,
    touches,
    changedTouches: [{clientX: x, clientY: y, identifier: 0}],
    stopImmediatePropagation() { this._stopped = true; },
    preventDefault() { this._prevented = true; }
  };
}

// A drag from (x0,y0) to (x1,y1) in `steps` moves over `ms` total.
function drag(g, x0, y0, x1, y1, ms, steps) {
  steps = steps || 8;
  const t0 = clock;
  g._feed(evt('touchstart', x0, y0, t0));
  for (let i = 1; i <= steps; i++) {
    const f = i / steps;
    g._feed(evt('touchmove', x0 + (x1 - x0) * f, y0 + (y1 - y0) * f, t0 + (ms * f), 1));
  }
  g._feed(evt('touchend', x1, y1, t0 + ms, 0));
  clock = t0 + ms + 1000;
}

function tap(g, x, y, atMs) {
  const t = atMs === undefined ? clock : atMs;
  g._feed(evt('touchstart', x, y, t));
  g._feed(evt('touchend', x, y, t + 30, 0));
  clock = t + 30;
  return t + 30;
}

function dirs(q) { return q.filter(e => e.g === 'arrow').map(e => e.dir); }

// THE DIRECTION CONVENTION, stated once because it is the easiest thing here to get backwards:
// content follows the finger, exactly as on mobile. Dragging the finger UP pulls the content up,
// which moves the selection DOWN the list — so a finger-up drag emits Down arrows. The arrow names
// the list movement, never the finger movement.
const OPPOSITE = {up: 'down', down: 'up', left: 'right', right: 'left'};

// ---- tests ----

// ---- the two hardware complaints, as regressions -------------------------------------------
//
// Tuned against a real panel on 2026-07-31. Both of these are about ONE number being asked to do
// two jobs: the stepPx that stops a normal swipe double-stepping is the same stepPx that makes a
// short swipe do nothing. The min-one-move rule splits the jobs, and these pin both halves.

function testOneSwipeIsExactlyOneMoveAtEveryDistance() {
  // "still some times it scroll x2, some times x1. We need to take action as x1."
  // Every distance a finger can travel on an 800px panel, fast and slow. There is no length of
  // swipe, and no speed, that produces two moves.
  for (const px of [22, 70, 120, 200, 340, 500, 720]) {
    for (const ms of [90, 220, 600, 1600]) {
      const {g} = load();
      drag(g, 640, 780, 640, 780 - px, ms, 8);
      const d = dirs(g.drain().q);
      eq(d, [OPPOSITE.up], `a ${px}px / ${ms}ms swipe must be exactly one move`);
    }
  }
}

function testHorizontalSwipesAreAlsoExactlyOne() {
  for (const px of [60, 250, 600]) {
    const {g} = load();
    drag(g, 1150, 400, 1150 - px, 400, 250, 8);
    eq(dirs(g.drain().q), [OPPOSITE.left], `a ${px}px sideways swipe is one move`);
  }
}

function testProportionalScrollingIsStillAvailable() {
  // The mechanism is not deleted, only defaulted off: maxSteps > 1 restores it.
  const {g} = load();
  g.configure({maxSteps: 5, stepPx: 100});
  drag(g, 640, 780, 640, 80, 1200, 12);   // 700 px
  const d = dirs(g.drain().q);
  check(d.length > 1 && d.length <= 5,
    `maxSteps:5 should scroll proportionally within the cap, got ${d.length}`);
}

function testAShortFlickStillMoves() {
  // "on iphone it's more sensitive" — raising stepPx to fix the double-step made short swipes emit
  // NOTHING, which reads as a dead panel. Any deliberate swipe past the slop is worth one move.
  for (const px of [22, 40, 70]) {
    const {g} = load();
    drag(g, 640, 500, 640, 500 - px, 120, 3);
    const d = dirs(g.drain().q);
    eq(d, [OPPOSITE.up], `a ${px}px flick must still move once`);
  }
}

function testMovementUnderTheSlopIsStillATap() {
  // The floor is the tap slop, not zero: a finger that barely wobbles is a tap, not a scroll.
  const {win, g} = load();
  g._feed(evt('touchstart', 640, 400, 1000));
  g._feed(evt('touchmove', 646, 404, 1030, 1));
  g._feed(evt('touchend', 646, 404, 1060, 0));
  eq(dirs(g.drain().q), [], 'a sub-slop wobble emits no arrow');
  win._fire(280);
  eq(g.drain().q.map(e => e.g), ['tap'], 'it is a tap');
}

function testTheFirstMoveLandsBeforeTheFingerLifts() {
  // One move per gesture must not mean "wait for release": on a long drag the move fires as soon
  // as the finger passes stepPx, so the panel answers immediately.
  const {g} = load();
  g._feed(evt('touchstart', 640, 700, 1000));
  g._feed(evt('touchmove', 640, 560, 1120, 1));   // 140 px, still down
  eq(dirs(g.drain().q), [OPPOSITE.up], 'the move arrives mid-gesture');
  g._feed(evt('touchmove', 640, 300, 1300, 1));   // keep going: no second move
  g._feed(evt('touchend', 640, 300, 1400, 0));
  eq(dirs(g.drain().q), [], 'and dragging further adds nothing');
}

function testDragDirectionMapping() {
  for (const [x0, y0, x1, y1, finger] of [
    [640, 700, 640, 300, 'up'],
    [640, 300, 640, 700, 'down'],
    [900, 400, 400, 400, 'left'],
    [400, 400, 900, 400, 'right'],
  ]) {
    const {g} = load();
    drag(g, x0, y0, x1, y1, 900, 10);   // slow: no momentum, so only the step arrows
    const want = OPPOSITE[finger];
    const d = dirs(g.drain().q);
    check(d.length > 0 && d.every(x => x === want),
      `a finger-${finger} drag should emit all '${want}', got ${JSON.stringify(d)}`);
  }
}

function testAxisLocksSoDiagonalsDoNotAlternate() {
  const {g} = load();
  // Mostly vertical with a persistent sideways component: must stay on one axis.
  drag(g, 640, 700, 760, 300, 900, 10);
  const d = dirs(g.drain().q);
  check(d.length > 0 && d.every(x => x === OPPOSITE.up),
    `a diagonal drag must not alternate axes, got ${JSON.stringify(d)}`);
}

function testMomentumCannotExceedTheGestureBudget() {
  // Raising flickMaxArrows must not smuggle extra moves past maxSteps.
  const {g} = load();
  g.configure({flickMaxArrows: 6});
  drag(g, 640, 780, 640, 180, 100, 4);   // 600 px in 100 ms: a hard flick
  eq(dirs(g.drain().q), [OPPOSITE.up], 'momentum is still bounded by maxSteps');
}

function testMomentumIsOffByDefault() {
  // A discrete-focus UI cannot glide: Leanback moves selection per key, so a momentum burst
  // overshoots. The mechanism stays, the default does not.
  const {g} = load();
  check(g._config().flickMaxArrows === 0, 'momentum ships off');
  drag(g, 640, 750, 640, 150, 120, 4);   // 600 px in 120 ms = 5 px/ms, a hard flick
  const d = dirs(g.drain().q);
  check(d.length === 1, `a hard flick is still one move, got ${d.length}`);
}

function testMomentumIsCappedWhenEnabled() {
  const {g} = load();
  g.configure({maxSteps: 20, flickMaxArrows: 6, stepPx: 200});
  drag(g, 640, 750, 640, 150, 120, 4);   // 600 px in 120 ms = 5 px/ms
  const d = dirs(g.drain().q);
  check(d.length > Math.floor(600 / 200), `enabled momentum should add arrows, got ${d.length}`);
  check(d.every(x => x === OPPOSITE.up), 'momentum continues the same list movement');

  const {g: g2} = load();
  g2.configure({maxSteps: 100, flickMaxArrows: 6, stepPx: 200});
  drag(g2, 640, 790, 640, 10, 40, 4);    // absurdly fast: 780 px in 40 ms
  const total = dirs(g2.drain().q).length;
  const cap = Math.floor(780 / 200) + 6;
  check(total <= cap, `momentum must be capped at ${cap}, got ${total} arrows`);
}

function testSlowDragGetsNoMomentum() {
  const {g} = load();
  drag(g, 640, 700, 640, 520, 2000, 6);  // 180 px over 2 s: steps only, no flick
  const d = dirs(g.drain().q);
  check(d.length > 0 && d.length <= 2 && d.every(x => x === OPPOSITE.up),
    `a slow drag emits step arrows only, got ${JSON.stringify(d)}`);
}

function testLeftEdgeSwipeIsBackAndNotAScroll() {
  const {g} = load();
  drag(g, 12, 400, 400, 400, 300, 8);
  const q = g.drain().q;
  eq(q.map(e => e.g), ['back'], 'a left-edge swipe right is Back, and emits no arrows');
}

function testEdgeSwipeThatDoesNotTravelFarEnoughDoesNothing() {
  const {g} = load();
  drag(g, 12, 400, 70, 400, 300, 6);   // 58 px < edgeMinPx
  eq(g.drain().q, [], 'a short edge drag is neither Back nor a scroll');
}

function testAVerticalDragFromTheEdgeStillScrolls() {
  const {g} = load();
  // Starting near the left edge must not disable scrolling — only the horizontal case is Back.
  drag(g, 12, 700, 12, 300, 900, 10);
  const d = dirs(g.drain().q);
  check(d.length > 0 && d.every(x => x === OPPOSITE.up),
    `a vertical drag at the edge is a normal scroll, got ${JSON.stringify(d)}`);
}

function testSingleTapWaitsForThePairingWindow() {
  const {win, g} = load();
  tap(g, 640, 400);
  eq(g.drain().q, [], 'a single tap does not fire until the double-tap window closes');
  win._fire(280);
  eq(g.drain().q.map(e => e.g), ['tap'], 'the single tap commits once the window expires');
}

function testDoubleTapLeftAndRightSeek() {
  for (const [x, want] of [[200, -1], [1100, 1]]) {
    const {win, g} = load();
    const t = tap(g, x, 400);
    tap(g, x, 400, t + 100);
    const q = g.drain().q;
    eq(q, [{g: 'seek', dir: want, n: 1}],
       `double-tap at x=${x} seeks ${want > 0 ? 'forward' : 'back'}`);
    win._fire(280);
    eq(g.drain().q, [], 'and the pending single tap was cancelled, so no stray Enter follows');
  }
}

function testDoubleTapInTheCentreIsATapNotASeek() {
  const {g} = load();
  const t = tap(g, 640, 400);
  tap(g, 640, 400, t + 100);
  eq(g.drain().q.map(e => e.g), ['tap'], 'the centre third has no seek zone');
}

function testTapsTooFarApartInTimeOrSpaceDoNotPair() {
  const {win, g} = load();
  const t = tap(g, 200, 400);
  tap(g, 200, 400, t + 400);            // outside doubleTapMs
  win._fire(280);
  const kinds = g.drain().q.map(e => e.g);
  check(!kinds.includes('seek'), `slow taps must not pair, got ${JSON.stringify(kinds)}`);

  const {win: w2, g: g2} = load();
  const t2 = tap(g2, 200, 400);
  tap(g2, 900, 400, t2 + 100);          // outside doubleTapSlopPx
  w2._fire(280);
  const kinds2 = g2.drain().q.map(e => e.g);
  check(!kinds2.includes('seek'), `distant taps must not pair, got ${JSON.stringify(kinds2)}`);
}

function testHoldDirectionComesFromTheZone() {
  // Mobile: hold the right third to run forward, the left to rewind. Taken from where the finger
  // went DOWN -- a hold does not move, but reading it at release would be a second source of truth.
  for (const [x, zone] of [[200, 'left'], [640, 'center'], [1100, 'right']]) {
    const {win, g} = load();
    g._feed(evt('touchstart', x, 400, 1000));
    win._fire(550);
    eq(g.drain().q, [{g: 'hold', on: true, zone}], `a hold at x=${x} is zone '${zone}'`);
  }
}

function testRepeatedDoubleTapsAccumulateLikeMobile() {
  // 10, 20, 30, 40 -- one growing jump, not four separate ones.
  const {g} = load();
  let t = 1000;
  const got = [];
  for (let i = 0; i < 4; i++) {
    tap(g, 1100, 400, t);
    tap(g, 1100, 400, t + 100);
    got.push(...g.drain().q.filter(e => e.g === 'seek').map(e => e.n));
    t += 400;                       // well inside seekRunMs
  }
  eq(got, [1, 2, 3, 4], 'each further double-tap on the same side extends the run');
}

function testTheRunResetsOnTheOtherSideAndOnTime() {
  const {g} = load();
  let t = 1000;
  tap(g, 1100, 400, t); tap(g, 1100, 400, t + 100);
  g.drain();
  // Other side: a new run, not a continuation -- otherwise a correction would jump 20s the wrong way.
  t += 400;
  tap(g, 200, 400, t); tap(g, 200, 400, t + 100);
  eq(g.drain().q.filter(e => e.g === 'seek').map(e => e.n), [1], 'the other side starts a new run');

  // And a pause longer than seekRunMs starts over.
  t += 3000;
  tap(g, 200, 400, t); tap(g, 200, 400, t + 100);
  eq(g.drain().q.filter(e => e.g === 'seek').map(e => e.n), [1], 'a pause ends the run');
}

function testLongPressHoldsAndReleases() {
  const {win, g} = load();
  g._feed(evt('touchstart', 640, 400, 1000));
  win._fire(550);
  eq(g.drain().q, [{g: 'hold', on: true, zone: 'center'}], 'holding still fires the hold gesture');
  g._feed(evt('touchend', 640, 400, 1800, 0));
  eq(g.drain().q, [{g: 'hold', on: false}], 'lifting always releases the hold');
}

function testMovingCancelsTheLongPress() {
  const {win, g} = load();
  g._feed(evt('touchstart', 640, 700, 1000));
  g._feed(evt('touchmove', 640, 500, 1100, 1));
  win._fire(550);
  const kinds = g.drain().q.map(e => e.g);
  check(!kinds.includes('hold'), `a drag must not become a hold, got ${JSON.stringify(kinds)}`);
}

function testALongPressDoesNotAlsoFireATap() {
  const {win, g} = load();
  g._feed(evt('touchstart', 640, 400, 1000));
  win._fire(550);
  g.drain();
  g._feed(evt('touchend', 640, 400, 1700, 0));
  const kinds = g.drain().q.map(e => e.g);
  eq(kinds, ['hold'], 'the release of a hold is not a tap');
}

function testMultiFingerIsInertAndCounted() {
  const {g} = load();
  g._feed(evt('touchstart', 400, 400, 1000, 1));
  g._feed(evt('touchstart', 800, 400, 1010, 2));   // second finger down
  g._feed(evt('touchmove', 800, 200, 1100, 2));
  g._feed(evt('touchend', 800, 200, 1200, 1));
  g._feed(evt('touchend', 400, 400, 1210, 0));
  eq(g.drain().q, [], 'a two-finger sequence emits nothing — multitouch delivery is unverified');
  check(g.stats().multiFinger > 0, 'and it is counted, so real use can prove delivery');
}

function testEveryEventIsSwallowed() {
  const {g} = load();
  const e = evt('touchstart', 640, 400, 1000);
  g._feed(e);
  check(e._stopped && e._prevented,
    'touch events must be stopped AND default-prevented so Leanback cannot double-act');
}

function testDisabledEmitsNothingButStillSwallows() {
  const {g} = load();
  g.setEnabled(false);
  const e = evt('touchstart', 640, 400, 1000);
  g._feed(e);
  drag(g, 640, 700, 640, 300, 300, 6);
  eq(g.drain().q, [], 'a disabled router emits nothing');
  check(e._stopped, 'but it still swallows, so a stray finger cannot navigate while off');
}

function testDrainReportsConfiguredSoAReloadCanBeDetected() {
  const {g} = load();
  check(g.drain().configured === false, 'a fresh instance reports configured:false');
  g.configure({stepPx: 120});
  check(g.drain().configured === true, 'and true once the launcher has pushed thresholds');
  check(g._config().stepPx === 120, 'configure applies known keys');
}

function testConfigureRejectsUnknownAndMistypedKeys() {
  const {g} = load();
  const before = g._config().stepPx;
  g.configure({stepPx: 'lots', nope: 5});
  check(g._config().stepPx === before, 'a wrongly-typed value is ignored, not coerced');
  check(!('nope' in g._config()), 'unknown keys are not added');
}

function testQueueIsBounded() {
  const {g} = load();
  g.configure({maxQueue: 4});
  for (let i = 0; i < 20; i++) drag(g, 640, 700, 640, 300, 900, 6);
  const r = g.drain();
  check(r.q.length <= 4, `queue must stay bounded, got ${r.q.length}`);
  check(r.stats.dropped > 0, 'and dropped events are counted rather than silently lost');
}

const tests = [
  testDragDirectionMapping,
  testAxisLocksSoDiagonalsDoNotAlternate,
  testOneSwipeIsExactlyOneMoveAtEveryDistance,
  testHorizontalSwipesAreAlsoExactlyOne,
  testProportionalScrollingIsStillAvailable,
  testAShortFlickStillMoves,
  testMovementUnderTheSlopIsStillATap,
  testTheFirstMoveLandsBeforeTheFingerLifts,
  testMomentumCannotExceedTheGestureBudget,
  testMomentumIsOffByDefault,
  testMomentumIsCappedWhenEnabled,
  testSlowDragGetsNoMomentum,
  testLeftEdgeSwipeIsBackAndNotAScroll,
  testEdgeSwipeThatDoesNotTravelFarEnoughDoesNothing,
  testAVerticalDragFromTheEdgeStillScrolls,
  testSingleTapWaitsForThePairingWindow,
  testDoubleTapLeftAndRightSeek,
  testDoubleTapInTheCentreIsATapNotASeek,
  testTapsTooFarApartInTimeOrSpaceDoNotPair,
  testHoldDirectionComesFromTheZone,
  testRepeatedDoubleTapsAccumulateLikeMobile,
  testTheRunResetsOnTheOtherSideAndOnTime,
  testLongPressHoldsAndReleases,
  testMovingCancelsTheLongPress,
  testALongPressDoesNotAlsoFireATap,
  testMultiFingerIsInertAndCounted,
  testEveryEventIsSwallowed,
  testDisabledEmitsNothingButStillSwallows,
  testDrainReportsConfiguredSoAReloadCanBeDetected,
  testConfigureRejectsUnknownAndMistypedKeys,
  testQueueIsBounded,
];

for (const t of tests) {
  clock = 1000;
  try {
    t();
  } catch (e) {
    failures++;
    console.error(`FAIL: ${t.name} threw ${e && e.stack ? e.stack : e}`);
  }
}

if (failures) {
  console.error(`touch_gestures.test.js: ${failures} failure(s)`);
  process.exit(1);
}
console.log(`touch_gestures.test.js: all ${tests.length} cases passed`);
