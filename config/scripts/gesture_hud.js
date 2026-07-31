// The on-screen indicator for touch gestures: mobile YouTube's "10 seconds" ripple and its "2x"
// pill, in one node. A one-shot — ScriptLibrary.render() appends {"text":...,"side":...,"ms":...};
// `ms` of 0 pins it until something hides it (what a press-and-hold needs), and an empty `text`
// hides it.
//
// Why a gesture needs one at all: touch has no key to look at and no button to feel. A seek that
// lands with no indicator is indistinguishable from a mis-tap, and a hold that changes the rate is
// invisible until the audio pitches. input-ux §4 makes this policy for the touch LOCK; the same
// argument covers every touch action, which is why the toggle already toasts.
//
// The three page constraints, all learned the hard way (m114.md, self-update.md):
//   * textContent, never innerHTML — youtube.com/tv enforces Trusted Types, and a bare innerHTML
//     assignment is a SILENT no-op there;
//   * CSSOM (.style.setProperty), never a style attribute — its CSP style-src has no
//     'unsafe-inline', so an inline style is dropped and the node renders unstyled;
//   * documentElement, not body — Leanback swaps body content on navigation.
(function (p) {
  var id = '__deckback_gesture_hud';
  var d = document.getElementById(id);
  if (!d) {
    d = document.createElement('div');
    d.id = id;
    document.documentElement.appendChild(d);
  }

  if (window.__deckbackHudT) { clearTimeout(window.__deckbackHudT); window.__deckbackHudT = null; }

  var text = p && p.text ? String(p.text) : '';
  if (!text) {                       // hide
    d.style.setProperty('opacity', '0');
    return true;
  }
  d.textContent = text;

  var s = d.style;
  s.setProperty('position', 'fixed');
  s.setProperty('top', '50%');
  s.setProperty('z-index', '2147483646');   // just under the toast, which is a higher-priority say
  s.setProperty('transform', 'translateY(-50%)');
  // Mobile puts the indicator over the half you touched, which is most of what makes it readable at
  // a glance: you already know which side you pressed, so the position confirms the direction
  // before you have read the number.
  var side = p && p.side ? p.side : 'center';
  s.setProperty('left', side === 'left' ? '12%' : (side === 'right' ? 'auto' : '50%'));
  s.setProperty('right', side === 'right' ? '12%' : 'auto');
  if (side === 'center') s.setProperty('transform', 'translate(-50%,-50%)');

  s.setProperty('background', 'rgba(0,0,0,0.72)');
  s.setProperty('color', '#fff');
  s.setProperty('font', '600 34px/1.2 system-ui,sans-serif');
  s.setProperty('padding', '20px 34px');
  s.setProperty('border-radius', '999px');
  s.setProperty('pointer-events', 'none');
  s.setProperty('white-space', 'pre');
  s.setProperty('text-align', 'center');
  s.setProperty('opacity', '1');
  s.setProperty('transition', 'opacity .18s ease-out');

  var ms = p && typeof p.ms === 'number' ? p.ms : 900;
  if (ms > 0) {
    window.__deckbackHudT = setTimeout(function () {
      var n = document.getElementById(id);
      if (n) n.style.setProperty('opacity', '0');
    }, ms);
  }
  return true;
})
