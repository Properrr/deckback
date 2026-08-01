// Touch gesture router — the page-layer half of P12.4. THE source of truth: the launcher compiles
// this in (CMake generates the script registry from config/scripts/*.js) and installs it at
// document-start (Page.addScriptToEvaluateOnNewDocument) when config `touch_gestures` is set.
//
// Why the page and not evdev. input-ux §11 priced a gesture layer as an EVIOCGRAB commitment:
// grabbing the panel would starve gamescope, making tap-to-activate, palm rejection and the
// coordinate transform all ours, and a launcher crash would kill touch entirely. That analysis is
// DEAD twice over — touch-lock.md proved on hardware that the launcher (as the seat user) cannot
// even open the panel node, and the 2026-07-31 touch probe proved that with gamescope's touch mode
// at 4 (passthrough) REAL wl_touch reaches Blink: a finger produces touchstart/touchmove/touchend
// with per-finger identifiers and page-space coordinates. So the router is an ordinary capture-phase
// listener, it needs no privileges, and a crash leaves touch exactly as it found it.
//
// Note this contradicts no_pointer.js's header, which says a finger "produces pointerdown/click,
// never touchstart". That was true of the mode the app pins (hover, 0) — not of the panel.
//
// Mechanism. Capture-phase listeners registered at document-start run before any Leanback handler;
// every touch event we consume gets stopImmediatePropagation() + preventDefault(), so Leanback
// never sees the finger and cannot double-act on it. Recognised gestures are pushed onto a queue
// that the launcher drains over CDP and turns into TRUSTED key events — a page cannot synthesize
// those (m114.md), which is why this file decides WHAT happened and never what to do about it.
//
// Gesture set, from input-ux §3 (mobile's spatial model) and §7 (the gaps vs mobile YouTube):
//
//   drag/swipe        -> exactly ONE move per gesture (maxSteps), fired as soon as the finger
//                        passes stepPx or on release if it never does. Leanback has no pointer
//                        scroll, so rails longer than a screen are otherwise untraversable by
//                        touch — the Switch YouTube app's most-criticised gap.
//
//                        Tuned on hardware 2026-07-31, in two passes, because the first two
//                        designs each failed in a way only a finger reveals. Proportional scrolling
//                        (one arrow per stepPx) is wrong here: Leanback moves SELECTION per key and
//                        cannot interpolate, so "distance travelled" has no continuous meaning and
//                        a long swipe silently double-steps. And stepPx alone cannot be tuned out
//                        of that, because the value big enough to stop a normal swipe
//                        double-stepping is the value that makes a short swipe do nothing at all.
//                        So: one gesture is one move, and stepPx only decides WHEN within the
//                        gesture it fires. maxSteps > 1 restores the proportional model.
//   flick             -> optional momentum arrows (default OFF: on a discrete-focus UI they
//                        overshoot rather than glide)
//   left-edge swipe   -> Escape. There is no touch Back path at all otherwise.
//   double-tap L/R    -> seek -/+ (mobile's left-half/right-half model). Repeating it on the same
//                        side ACCUMULATES like mobile: 10, 20, 30, 40s, not four separate jumps.
//   single tap        -> Enter (activate tile / toggle controls)
//   long press L/R/C  -> hold-to-scrub while held, direction from the zone: right runs forward at
//                        2x, left rewinds. Released on lift.
//
// MULTITOUCH IS INERT HERE, BUT IT IS DELIVERED. Any sequence that ever sees more than one finger
// is abandoned and counted in stats().multiFinger, and no gesture comes out of it. That counter was
// built as a self-probe — and on 2026-07-31 it CLIMBED during ordinary use (5 in 48 sequences), on
// a device whose navigator.maxTouchPoints reads 0. So gamescope -> Xwayland -> XI2 -> Blink does
// carry multitouch, and pinch/two-finger work is unblocked.
//
// Still unknown, and required before designing one: HOW MANY points arrive. This abandons on the
// SECOND finger, so it has never counted past 2, and it does not check that coordinates stay sane
// with two down. Raise the abandon threshold and read maxTouches first.
(function () {
  var W = window;
  if (W.__deckbackGestures && W.__deckbackGestures.uninstall) {
    try { W.__deckbackGestures.uninstall(); } catch (_) {}
  }

  var DEFAULTS = {
    enabled: true,
    stepPx: 70,           // travel at which a drag's move fires (short swipes fire on release)
    maxSteps: 1,          // moves per gesture. 1 = one swipe is one move, however far it travels
    tapSlopPx: 16,        // movement under this still counts as a tap, not a drag
    tapMaxMs: 400,        // a touch held longer than this is not a tap
    doubleTapMs: 280,     // second tap must land within this to pair
    doubleTapSlopPx: 120, // ...and within this distance of the first
    edgePx: 40,           // a touch starting this close to the left edge may be a Back swipe
    edgeMinPx: 90,        // ...and must travel at least this far right to be one
    flickMinVel: 0.6,     // px/ms at release to add momentum arrows
    flickMaxArrows: 0,    // momentum arrows; 0 = off (a discrete-focus UI overshoots, see onEnd)
    momentumFactor: 3.0,  // arrows per px/ms of release velocity
    longPressMs: 550,     // hold this long without moving to trigger the held-scrub gesture
    seekRunMs: 1400,      // a further double-tap within this ACCUMULATES the seek (10,20,30,40s)
    maxQueue: 64          // a queue nobody drains must not grow without bound
  };

  var cfg = {};
  for (var k in DEFAULTS) cfg[k] = DEFAULTS[k];

  var queue = [];
  var stats = {multiFinger: 0, sequences: 0, dropped: 0, emitted: 0};
  var configured = false;

  var push = function (g) {
    if (!cfg.enabled) return;
    if (queue.length >= cfg.maxQueue) { queue.shift(); stats.dropped++; }
    queue.push(g);
    stats.emitted++;
  };

  // ---- one touch sequence ----
  var seq = null;
  var lastTap = null;      // {x, y, t} of the previous completed tap, for double-tap pairing
  var pendingTap = null;   // setTimeout id: a single tap not yet committed
  // Mobile's accumulating seek: keep double-tapping the same side and the jump grows 10, 20, 30,
  // 40s rather than firing four separate 10s jumps. {zone, n, t} of the run in progress.
  var seekRun = null;

  var now = function (e) {
    return (e && typeof e.timeStamp === 'number') ? e.timeStamp : Date.now();
  };

  var point = function (e) {
    var t = (e.changedTouches && e.changedTouches[0]) || (e.touches && e.touches[0]);
    return t ? {x: t.clientX, y: t.clientY} : null;
  };

  var fingers = function (e) {
    return (e.touches && typeof e.touches.length === 'number') ? e.touches.length : 1;
  };

  var zoneOf = function (x) {
    var w = W.innerWidth || 1280;
    if (x < w / 3) return 'left';
    if (x > (w * 2) / 3) return 'right';
    return 'center';
  };

  // Content follows the finger: dragging up moves DOWN the list, as on mobile. `delta` is the
  // travel along `axis`; the arrow names the LIST movement, never the finger movement. One place,
  // because the live steps and the release both need it and they must never disagree.
  var arrowFor = function (axis, delta) {
    var forward = delta > 0;
    return axis === 'x' ? (forward ? 'left' : 'right') : (forward ? 'up' : 'down');
  };

  var commitTap = function (x, y) {
    pendingTap = null;
    push({g: 'tap', zone: zoneOf(x), x: Math.round(x), y: Math.round(y)});
  };

  var onStart = function (e) {
    stats.sequences++;
    if (fingers(e) > 1) {
      // More than one finger: abandon the sequence rather than guess. See the header.
      stats.multiFinger++;
      if (seq) seq.abandoned = true;
      return;
    }
    var p = point(e);
    if (!p) return;
    var t = now(e);
    seq = {
      x0: p.x, y0: p.y, t0: t,
      x: p.x, y: p.y, t: t,
      accum: 0, axis: null, moved: false, abandoned: false,
      edge: p.x <= cfg.edgePx, longFired: false, emitted: 0
    };
    // A long press is only meaningful while the finger stays put; onMove cancels it.
    seq.longTimer = W.setTimeout(function () {
      if (seq && !seq.moved && !seq.abandoned) {
        seq.longFired = true;
        // The zone decides the DIRECTION, as on mobile: hold the right third to run forward, the
        // left third to rewind. Captured from where the finger went down, not where it lifts.
        push({g: 'hold', on: true, zone: zoneOf(seq.x0)});
      }
    }, cfg.longPressMs);
  };

  var onMove = function (e) {
    if (!seq || seq.abandoned) return;
    if (fingers(e) > 1) { stats.multiFinger++; seq.abandoned = true; return; }
    var p = point(e);
    if (!p) return;
    var dx = p.x - seq.x, dy = p.y - seq.y;
    seq.x = p.x; seq.y = p.y; seq.t = now(e);

    var totalDx = Math.abs(p.x - seq.x0), totalDy = Math.abs(p.y - seq.y0);
    if (!seq.moved && (totalDx > cfg.tapSlopPx || totalDy > cfg.tapSlopPx)) {
      seq.moved = true;
      if (seq.longTimer) { W.clearTimeout(seq.longTimer); seq.longTimer = null; }
    }
    if (!seq.moved) return;

    // Lock the axis on first real movement so a slightly diagonal drag does not alternate between
    // horizontal and vertical arrows.
    if (!seq.axis) seq.axis = totalDx > totalDy ? 'x' : 'y';

    // A sequence that began in the left edge zone might still become a Back swipe, so it emits
    // nothing until release — otherwise Back would also scroll the rail it swiped across.
    if (seq.edge && seq.axis === 'x') return;

    seq.accum += seq.axis === 'x' ? dx : dy;
    while (Math.abs(seq.accum) >= cfg.stepPx && seq.emitted < cfg.maxSteps) {
      var dir = arrowFor(seq.axis, seq.accum);
      seq.accum -= seq.accum > 0 ? cfg.stepPx : -cfg.stepPx;
      push({g: 'arrow', dir: dir});
      seq.emitted++;
    }
  };

  var onEnd = function (e) {
    if (!seq) return;
    // Only the LAST finger up ends the sequence; earlier lifts of a multi-finger touch do not, and
    // must not cancel the long-press timer either.
    if (fingers(e) > 0) return;
    var s = seq;
    if (s.longTimer) { W.clearTimeout(s.longTimer); s.longTimer = null; }
    seq = null;
    if (s.abandoned) { if (s.longFired) push({g: 'hold', on: false}); return; }

    var p = point(e) || {x: s.x, y: s.y};
    var dx = p.x - s.x0, dy = p.y - s.y0;
    var dt = Math.max(1, now(e) - s.t0);

    if (s.longFired) { seekRun = null; push({g: 'hold', on: false}); return; }

    if (!s.moved) {
      if (dt > cfg.tapMaxMs) return;  // a long hold that did not reach longPressMs: not a tap
      var paired = lastTap &&
        (now(e) - lastTap.t) <= cfg.doubleTapMs &&
        Math.abs(p.x - lastTap.x) <= cfg.doubleTapSlopPx &&
        Math.abs(p.y - lastTap.y) <= cfg.doubleTapSlopPx;
      if (paired) {
        if (pendingTap) { W.clearTimeout(pendingTap); pendingTap = null; }
        lastTap = null;
        var zone = zoneOf(p.x);
        if (zone === 'left' || zone === 'right') {
          // Same side, soon enough: extend the run. Otherwise start a new one. The launcher
          // multiplies its skip interval by `n`, so the page never needs to know the seconds.
          var t = now(e);
          if (seekRun && seekRun.zone === zone && (t - seekRun.t) <= cfg.seekRunMs) seekRun.n++;
          else seekRun = {zone: zone, n: 1, t: t};
          seekRun.t = t;
          push({g: 'seek', dir: zone === 'right' ? 1 : -1, n: seekRun.n});
        } else {
          seekRun = null;
          push({g: 'tap', zone: 'center', x: Math.round(p.x), y: Math.round(p.y)});
        }
        return;
      }
      lastTap = {x: p.x, y: p.y, t: now(e)};
      // Wait out the pairing window before committing a single tap, so a double-tap seek does not
      // also fire an Enter. This is the same latency mobile YouTube has, for the same reason.
      var cx = p.x, cy = p.y;
      pendingTap = W.setTimeout(function () { commitTap(cx, cy); }, cfg.doubleTapMs);
      return;
    }

    // A swipe that began at the left edge and travelled right is Back, never a scroll.
    if (s.edge && dx >= cfg.edgeMinPx && Math.abs(dx) > Math.abs(dy)) {
      push({g: 'back'});
      return;
    }
    if (s.edge && s.axis === 'x') return;  // an edge drag that did not qualify does nothing

    // ANY deliberate swipe moves at least once. Without this, a flick shorter than stepPx emits
    // nothing at all and the panel feels dead -- the exact complaint that stepPx alone cannot fix,
    // because raising it to stop a normal swipe double-stepping is what makes a short one do
    // nothing. So stepPx governs only the ADDITIONAL moves of a longer drag, and travelling past
    // the tap slop is always worth one.
    if (s.emitted === 0) {
      var axis = s.axis || (Math.abs(dx) > Math.abs(dy) ? 'x' : 'y');
      push({g: 'arrow', dir: arrowFor(axis, axis === 'x' ? dx : dy)});
      s.emitted++;
    }

    // Momentum: a fast release keeps the list moving, as a flick does on mobile. Default 0 -- on a
    // DISCRETE-focus UI a burst of arrows overshoots rather than glides, because Leanback moves
    // selection per key and cannot interpolate. Kept configurable, not deleted. Also bounded by the
    // per-gesture budget, so raising it cannot smuggle extra moves past maxSteps.
    if (s.emitted >= cfg.maxSteps) return;
    var vel = Math.sqrt(dx * dx + dy * dy) / dt;
    if (vel >= cfg.flickMinVel) {
      var n = Math.min(cfg.flickMaxArrows, Math.round(vel * cfg.momentumFactor),
                       cfg.maxSteps - s.emitted);
      var horizontal = s.axis === 'x';
      var forward = horizontal ? dx < 0 : dy < 0;
      for (var i = 0; i < n; i++) {
        push({g: 'arrow', dir: horizontal ? (forward ? 'right' : 'left')
                                          : (forward ? 'down' : 'up')});
      }
    }
  };

  var onCancel = function () {
    if (seq && seq.longTimer) W.clearTimeout(seq.longTimer);
    if (seq && seq.longFired) push({g: 'hold', on: false});
    seq = null;
  };

  var swallow = function (e) {
    try { e.stopImmediatePropagation(); } catch (_) {}
    try { if (e.cancelable) e.preventDefault(); } catch (_) {}
  };

  var handler = function (e) {
    // Swallow first, so an exception in recognition can never leak the event to Leanback and cause
    // a double actuation. Recognition failing silently is a missed gesture; leaking is a wrong one.
    swallow(e);
    if (!cfg.enabled) return;
    try {
      if (e.type === 'touchstart') onStart(e);
      else if (e.type === 'touchmove') onMove(e);
      else if (e.type === 'touchend') onEnd(e);
      else onCancel(e);
    } catch (_) {}
  };

  // The compatibility mouse/pointer events Blink synthesizes from touch are swallowed too: with
  // preventDefault() on the touch they should not arrive at all, but if any does it must not reach
  // Leanback as a stray click.
  var TOUCH = ['touchstart', 'touchmove', 'touchend', 'touchcancel'];
  var COMPAT = ['mousedown', 'mouseup', 'mousemove', 'click', 'dblclick', 'auxclick', 'contextmenu',
                'pointerdown', 'pointerup', 'pointermove', 'pointercancel'];
  var targets = [W, W.document];
  var bound = [];

  var install = function () {
    for (var i = 0; i < targets.length; i++) {
      var t = targets[i];
      if (!t || !t.addEventListener) continue;
      for (var j = 0; j < TOUCH.length; j++) {
        try { t.addEventListener(TOUCH[j], handler, {capture: true, passive: false}); } catch (_) {}
        bound.push([t, TOUCH[j], handler]);
      }
      for (var m = 0; m < COMPAT.length; m++) {
        try { t.addEventListener(COMPAT[m], swallow, {capture: true, passive: false}); } catch (_) {}
        bound.push([t, COMPAT[m], swallow]);
      }
    }
  };

  W.__deckbackGestures = {
    version: 2,
    // The launcher drains this. `configured` tells it whether THIS instance has been given the
    // user's thresholds yet: a page reload installs a fresh document-start copy with defaults, and
    // the launcher has no reload signal it can trust, so the answer travels with every drain.
    drain: function () {
      var out = queue;
      queue = [];
      return {v: 1, configured: configured, enabled: !!cfg.enabled, q: out, stats: stats};
    },
    configure: function (p) {
      if (p) for (var key in p) if (key in cfg && typeof p[key] === typeof cfg[key]) cfg[key] = p[key];
      configured = true;
      return true;
    },
    setEnabled: function (on) {
      cfg.enabled = !!on;
      if (!cfg.enabled) {
        queue = [];
        if (seq && seq.longTimer) W.clearTimeout(seq.longTimer);
        if (pendingTap) { W.clearTimeout(pendingTap); pendingTap = null; }
        seq = null;
      }
      return cfg.enabled;
    },
    stats: function () { return stats; },
    // Test seam: drive recognition without a DOM. tests/js/touch_gestures.test.js uses this.
    _feed: function (e) { handler(e); },
    _reset: function () {
      queue = []; seq = null; lastTap = null; seekRun = null;
      if (pendingTap) { W.clearTimeout(pendingTap); pendingTap = null; }
      stats = {multiFinger: 0, sequences: 0, dropped: 0, emitted: 0};
    },
    _config: function () { return cfg; },
    uninstall: function () {
      for (var i = 0; i < bound.length; i++) {
        try { bound[i][0].removeEventListener(bound[i][1], bound[i][2], true); } catch (_) {}
      }
      bound = [];
      if (seq && seq.longTimer) W.clearTimeout(seq.longTimer);
      if (pendingTap) W.clearTimeout(pendingTap);
      seq = null; pendingTap = null;
    }
  };

  install();
})();
