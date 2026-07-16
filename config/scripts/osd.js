// OSD Settings menu (osd-menu-plan.md). Invoked via p.op and returns a string over eval_string:
//   open  -> build from model params, "ok"        close -> tear down, "closed"
//   cmd   -> apply p.cmd, verdict "consumed"|"close"|"action:<id>"|"gone"   state -> "tab=..;idx=.."
// Component lives on window.__dbOSD. CSP-safe (adoptedStyleSheets + CSSOM only, no style element or
// inline style attribute, no innerHTML) and keep-alive'd like the other launcher overlays.
(function (p) {
  var ID = '__deckback_osd';

  // keep-alive (shared, identical across launcher overlays)
  if (!window.__dbKeepAlive) {
    window.__dbKeep = window.__dbKeep || [];
    window.__dbKeepAlive = function (node) {
      if (window.__dbKeep.indexOf(node) === -1) window.__dbKeep.push(node);
      if (!window.__dbKeepObs && window.MutationObserver && document.documentElement) {
        try {
          window.__dbKeepObs = new MutationObserver(function () {
            var r = window.__dbKeep || [];
            for (var i = 0; i < r.length; i++) {
              try {
                if (r[i] && !r[i].isConnected) document.documentElement.appendChild(r[i]);
              } catch (_) {}
            }
          });
          window.__dbKeepObs.observe(document.documentElement, { childList: true });
        } catch (_) {}
      }
    };
    window.__dbDropAlive = function (node) {
      var r = window.__dbKeep || [];
      var i = r.indexOf(node);
      if (i !== -1) r.splice(i, 1);
    };
  }

  var RULES =
    "#__deckback_osd{position:fixed;inset:0;z-index:2147483646;background:rgba(8,8,8,0.95);" +
    "color:#fff;display:flex;flex-direction:column;gap:18px;padding:5vh 7vw;box-sizing:border-box;" +
    "font:400 22px/1.4 system-ui,Roboto,Arial,sans-serif;}" +
    "#__deckback_osd .tabs{display:flex;gap:10px;border-bottom:2px solid rgba(255,255,255,0.12);" +
    "padding-bottom:10px;}" +
    "#__deckback_osd .tab{padding:8px 22px;border-radius:10px 10px 0 0;color:#bdbdbd;" +
    "font-weight:600;display:flex;align-items:center;gap:10px;}" +
    "#__deckback_osd .tab.on{color:#fff;background:rgba(255,255,255,0.08);}" +
    "#__deckback_osd .tab .badge{width:11px;height:11px;border-radius:50%;background:#f5b301;" +
    "box-shadow:0 0 6px 2px rgba(245,179,1,0.5);}" +
    "#__deckback_osd .content{display:flex;flex:1 1 auto;gap:28px;min-height:0;}" +
    "#__deckback_osd .panel{display:none;flex:1 1 auto;flex-direction:column;gap:16px;min-height:0;}" +
    "#__deckback_osd .panel.on{display:flex;}" +
    "#__deckback_osd .rail{display:flex;flex-direction:column;gap:8px;flex:0 0 auto;}" +
    "#__deckback_osd .subtab{padding:8px 20px;border-radius:9px;color:#bdbdbd;font-weight:600;}" +
    "#__deckback_osd .subtab.on{color:#fff;background:rgba(255,255,255,0.10);}" +
    // Injected overlays cannot use native scrolling: youtube.com/tv resets scrollTop on any element
    // we add (verified on-Deck). So .scroll clips (overflow:hidden) and we translate an inner wrapper
    // ourselves, drawing our own thumb. inset:0-style height cap comes from each region's own rule.
    "#__deckback_osd .scroll{position:relative;overflow:hidden;min-height:0;}" +
    "#__deckback_osd .sinner{will-change:transform;padding-right:16px;}" +
    "#__deckback_osd .sbar{position:absolute;top:0;right:0;width:8px;height:100%;" +
    "background:rgba(255,255,255,0.07);border-radius:4px;}" +
    "#__deckback_osd .sthumb{position:absolute;right:0;width:8px;border-radius:4px;" +
    "background:rgba(255,255,255,0.34);}" +
    "#__deckback_osd table{border-collapse:collapse;font-size:24px;}" +
    "#__deckback_osd td{padding:8px 24px;}" +
    "#__deckback_osd td.k{text-align:right;color:#9fd0ff;font-weight:600;white-space:nowrap;}" +
    "#__deckback_osd td.v{text-align:left;color:#eee;}" +
    "#__deckback_osd .status{font-size:30px;font-weight:600;color:#9fd0ff;margin-bottom:4px;}" +
    "#__deckback_osd .keys{max-height:62vh;}" +
    "#__deckback_osd .notes{max-height:44vh;max-width:64ch;}" +
    "#__deckback_osd .notes .sinner{white-space:pre-wrap;text-align:left;line-height:1.75;" +
    "background:rgba(255,255,255,0.06);border-radius:14px;padding:22px 30px;font-size:25px;" +
    "color:#efefef;}" +
    "#__deckback_osd .about{max-height:66vh;max-width:74ch;}" +
    "#__deckback_osd .a-title{font-size:34px;font-weight:700;color:#fff;}" +
    "#__deckback_osd .a-summary{font-size:24px;color:#9fd0ff;margin:2px 0 14px;}" +
    "#__deckback_osd .a-desc{font-size:22px;line-height:1.6;color:#e6e6e6;}" +
    "#__deckback_osd .a-head{font-size:20px;font-weight:700;letter-spacing:0.06em;" +
    "text-transform:uppercase;color:#9a9a9a;margin:20px 0 8px;}" +
    "#__deckback_osd .a-feats{margin:0;padding-left:26px;}" +
    "#__deckback_osd .a-feats li{font-size:22px;line-height:1.5;color:#eaeaea;padding:4px 0;}" +
    "#__deckback_osd .a-meta{margin-top:22px;padding-top:14px;font-size:20px;color:#c8c8c8;" +
    "border-top:2px solid rgba(255,255,255,0.12);display:flex;flex-direction:column;gap:6px;}" +
    "#__deckback_osd .a-lk{color:#9a9a9a;}" +
    "#__deckback_osd .a-lv{color:#9fd0ff;word-break:break-all;}" +
    "#__deckback_osd .actions{display:flex;gap:16px;flex-wrap:wrap;}" +
    "#__deckback_osd .btn{padding:12px 26px;border-radius:12px;border:2px solid rgba(255,255,255,0.18);" +
    "background:rgba(255,255,255,0.05);color:#fff;font-weight:600;font-size:22px;}" +
    "#__deckback_osd .dbf{outline:3px solid #f5b301;outline-offset:2px;background:rgba(245,179,1,0.14);}" +
    "#__deckback_osd .hint{color:#9a9a9a;font-size:19px;border-top:2px solid rgba(255,255,255,0.12);" +
    "padding-top:10px;}" +
    "#__deckback_osd .hint .kb{font-weight:700;}";

  function ensureSheet() {
    try {
      if (!('adoptedStyleSheets' in document) || !window.CSSStyleSheet) return;
      if (!window.__dbOsdSheet) window.__dbOsdSheet = new CSSStyleSheet();
      // Always refresh: a cached sheet from an older osd.js injection would otherwise pin stale CSS.
      window.__dbOsdSheet.replaceSync(RULES);
      var have = document.adoptedStyleSheets || [];
      if (have.indexOf(window.__dbOsdSheet) === -1)
        document.adoptedStyleSheets = [].concat(have, window.__dbOsdSheet);
    } catch (_) {}
  }

  function el(tag, cls, text) {
    var n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;
    return n;
  }

  // A clip viewport (.scroll) whose content lives in .sinner (translated by us) with a drawn thumb.
  // Callers append content to box.__inner. Height is bounded by the caller's own CSS class.
  function scrollBox(extraCls) {
    var box = el('div', 'scroll' + (extraCls ? ' ' + extraCls : ''));
    var inner = el('div', 'sinner');
    var bar = el('div', 'sbar');
    var thumb = el('div', 'sthumb');
    bar.appendChild(thumb);
    box.appendChild(inner);
    box.appendChild(bar);
    box.__inner = inner;
    box.__thumb = thumb;
    box.__off = 0;
    return box;
  }

  function applyScroll(box) {
    var vh = box.clientHeight || 0;
    var ch = box.__inner ? (box.__inner.scrollHeight || box.__inner.offsetHeight) : 0;
    var max = Math.max(0, ch - vh);
    if (box.__off > max) box.__off = max;
    if (box.__off < 0) box.__off = 0;
    if (box.__inner) box.__inner.style.setProperty('transform', 'translateY(' + -box.__off + 'px)');
    var thumb = box.__thumb;
    if (!thumb) return;
    if (max <= 0) {
      thumb.style.setProperty('display', 'none');
      return;
    }
    thumb.style.setProperty('display', 'block');
    var th = Math.max(24, Math.round((vh * vh) / ch));
    thumb.style.setProperty('height', th + 'px');
    thumb.style.setProperty('top', Math.round((box.__off / max) * (vh - th)) + 'px');
  }

  // ---- build (op:"open") ------------------------------------------------------------------------
  function build(p) {
    ensureSheet();

    var old = document.getElementById(ID);
    if (old) {
      if (window.__dbDropAlive) window.__dbDropAlive(old);
      old.remove();
    }

    var root = el('div');
    root.id = ID;
    // CSSOM fallback so the backdrop covers the page even without constructable stylesheets.
    var rs = root.style;
    rs.setProperty('position', 'fixed');
    rs.setProperty('inset', '0');
    rs.setProperty('z-index', '2147483646');
    rs.setProperty('display', 'flex');
    rs.setProperty('flex-direction', 'column');
    rs.setProperty('background', 'rgba(8,8,8,0.95)');
    rs.setProperty('color', '#fff');

    var hasUpdate = !!p.upd_has;

    // Tab strip. Order drives L1/R1 cycling.
    var order = ['settings', 'updates'];
    if (p.about_name) order.push('about');

    var tabs = el('div', 'tabs');
    var tabEls = {};
    function addTab(key, label, withBadge) {
      var t = el('div', 'tab');
      t.setAttribute('data-tab', key);
      t.appendChild(el('span', null, label));
      if (withBadge) t.appendChild(el('span', 'badge'));
      tabEls[key] = t;
      tabs.appendChild(t);
    }
    addTab('settings', 'Settings');
    addTab('updates', 'Updates', hasUpdate);
    if (p.about_name) addTab('about', 'About');

    var content = el('div', 'content');

    // Settings panel: sub-tab rail + Keys table (read-only).
    var pSettings = el('div', 'panel');
    pSettings.setAttribute('data-tab', 'settings');
    var rail = el('div', 'rail');
    var sKeys = el('div', 'subtab on', 'Keys');
    rail.appendChild(sKeys);
    var keysScroll = scrollBox('keys');
    var table = el('table');
    var rows = p.keys || [];
    for (var i = 0; i < rows.length; i++) {
      var tr = el('tr');
      tr.appendChild(el('td', 'k', rows[i][0]));
      tr.appendChild(el('td', 'v', rows[i][1]));
      table.appendChild(tr);
    }
    keysScroll.__inner.appendChild(table);
    pSettings.appendChild(rail);
    pSettings.appendChild(keysScroll);

    // Updates panel: status + changelog + action buttons.
    var pUpdates = el('div', 'panel');
    pUpdates.setAttribute('data-tab', 'updates');
    pUpdates.appendChild(el('div', 'status', p.upd_status || ''));
    if (p.upd_notes) {
      var notes = scrollBox('notes');
      notes.__inner.textContent = p.upd_notes;
      pUpdates.appendChild(notes);
    }
    var actions = el('div', 'actions');
    var btns = p.upd_buttons || [];  // [ [id,label], ... ]
    for (var j = 0; j < btns.length; j++) {
      var b = el('div', 'btn', btns[j][1]);
      b.setAttribute('data-focus', '1');
      b.setAttribute('data-action', btns[j][0]);
      actions.appendChild(b);
    }
    pUpdates.appendChild(actions);

    content.appendChild(pSettings);
    content.appendChild(pUpdates);

    // About panel: single-sourced from the AppStream metainfo (the launcher parses it and passes the
    // fields). Scrollable, since the feature list + description overflow on the 720p panel.
    var pAbout = null;
    if (p.about_name) {
      pAbout = el('div', 'panel');
      pAbout.setAttribute('data-tab', 'about');
      var aboutScroll = scrollBox('about');
      var ai = aboutScroll.__inner;
      ai.appendChild(el('div', 'a-title', p.about_name));
      if (p.about_summary) ai.appendChild(el('div', 'a-summary', p.about_summary));
      if (p.about_desc) ai.appendChild(el('div', 'a-desc', p.about_desc));
      var feats = p.about_features || [];
      if (feats.length) {
        ai.appendChild(el('div', 'a-head', 'Features'));
        var ul = el('ul', 'a-feats');
        for (var fi = 0; fi < feats.length; fi++) ul.appendChild(el('li', null, feats[fi]));
        ai.appendChild(ul);
      }
      var meta = el('div', 'a-meta');
      if (p.about_version) meta.appendChild(el('div', null, 'Version ' + p.about_version));
      if (p.about_author) meta.appendChild(el('div', null, 'By ' + p.about_author));
      var alinks = p.about_links || [];  // [ [label,url], ... ]
      for (var li2 = 0; li2 < alinks.length; li2++) {
        var row = el('div', 'a-link');
        row.appendChild(el('span', 'a-lk', alinks[li2][0] + ': '));
        row.appendChild(el('span', 'a-lv', alinks[li2][1]));
        meta.appendChild(row);
      }
      ai.appendChild(meta);
      pAbout.appendChild(aboutScroll);
      content.appendChild(pAbout);
    }

    var hint = el('div', 'hint');
    hint.appendChild(el('span', 'kb', 'A'));
    hint.appendChild(el('span', null, ' Select   '));
    hint.appendChild(el('span', 'kb', 'B'));
    hint.appendChild(el('span', null, ' Back   '));
    hint.appendChild(el('span', 'kb', 'L1/R1'));
    hint.appendChild(el('span', null, ' Switch tab   ↑↓ Move   R-stick Scroll'));

    root.appendChild(tabs);
    root.appendChild(content);
    root.appendChild(hint);
    document.documentElement.appendChild(root);
    if (window.__dbKeepAlive) window.__dbKeepAlive(root);

    var S = {
      root: root,
      order: order,
      tabs: tabEls,
      panels: { settings: pSettings, updates: pUpdates, about: pAbout },
      tab: 'settings',
      focusables: [],
      focusIdx: -1
    };

    S.clearRing = function () {
      if (S.focusables[S.focusIdx]) S.focusables[S.focusIdx].classList.remove('dbf');
    };
    S.applyRing = function () {
      var n = S.focusables[S.focusIdx];
      if (n) {
        n.classList.add('dbf');
        try { n.scrollIntoView({ block: 'nearest' }); } catch (_) {}
      }
    };
    S.collect = function () {
      var nodes = S.panels[S.tab].querySelectorAll('[data-focus]');
      S.focusables = Array.prototype.slice.call(nodes);
    };
    S.setTab = function (tab, idx) {
      S.clearRing();
      S.tab = (S.order.indexOf(tab) !== -1) ? tab : 'settings';
      for (var t = 0; t < S.order.length; t++) {
        var key = S.order[t];
        var on = key === S.tab;
        if (S.panels[key]) S.panels[key].classList.toggle('on', on);
        if (S.tabs[key]) S.tabs[key].classList.toggle('on', on);
      }
      S.collect();
      // first interactive widget, or -1 (rest on tab strip) when the panel has none
      if (typeof idx === 'number' && idx >= 0 && idx < S.focusables.length) S.focusIdx = idx;
      else S.focusIdx = S.focusables.length ? 0 : -1;
      S.applyRing();
      var box = S.panels[S.tab] ? S.panels[S.tab].querySelector('.scroll') : null;
      if (box) applyScroll(box);  // size/show the thumb for the tab we just entered
    };
    S.cycleTab = function (step) {
      var i = S.order.indexOf(S.tab);
      if (i === -1) i = 0;
      var n = (i + step + S.order.length) % S.order.length;
      S.setTab(S.order[n]);
    };
    S.move = function (dir) {
      if (!S.focusables.length) return 'consumed';
      S.clearRing();
      var n = S.focusIdx + dir;
      if (n < 0) n = 0;
      if (n > S.focusables.length - 1) n = S.focusables.length - 1;
      S.focusIdx = n;
      S.applyRing();
      return 'consumed';
    };
    S.activate = function () {
      var n = S.focusables[S.focusIdx];
      if (!n) return 'consumed';
      var a = n.getAttribute('data-action');
      return a ? ('action:' + a) : 'consumed';
    };
    S.scroll = function (dir) {
      var panel = S.panels[S.tab];
      var box = panel ? panel.querySelector('.scroll') : null;
      if (!box) return 'consumed';
      var step = Math.max(48, Math.round((box.clientHeight || 200) * 0.2));
      box.__off = (box.__off || 0) + dir * step;
      applyScroll(box);
      return 'consumed';
    };
    S.exec = function (cmd) {
      if (!S.root || !S.root.isConnected) return 'gone';
      switch (cmd) {
        case 'tab_next':
          S.cycleTab(1);
          return 'consumed';
        case 'tab_prev':
          S.cycleTab(-1);
          return 'consumed';
        case 'up': return S.focusables.length ? S.move(-1) : S.scroll(-1);
        case 'down': return S.focusables.length ? S.move(1) : S.scroll(1);
        case 'left':
        case 'right': return 'consumed';  // reserved for combobox value change (no widgets in v1)
        case 'select': return S.activate();
        case 'ignore':  // contextual Y: ignore the available update, only on the Updates tab
          if (S.tab === 'updates' && S.panels.updates.querySelector('[data-action="update.ignore"]'))
            return 'action:update.ignore';
          return 'consumed';
        case 'back': return 'close';       // no open combobox in v1: back always closes
        case 'scroll_up': return S.scroll(-1);
        case 'scroll_down': return S.scroll(1);
      }
      return 'consumed';
    };
    S.state = function () {
      if (!S.root || !S.root.isConnected) return 'gone';
      return 'tab=' + S.tab + ';idx=' + S.focusIdx;
    };
    S.close = function () {
      if (window.__dbDropAlive) window.__dbDropAlive(S.root);
      if (S.root) S.root.remove();
    };

    window.__dbOSD = S;
    S.setTab(p.tab === 'updates' ? 'updates' : 'settings',
             (typeof p.restore_idx === 'number') ? p.restore_idx : -1);
    return 'ok';
  }

  // ---- dispatch ---------------------------------------------------------------------------------
  var op = p.op || 'open';
  if (op === 'open') return build(p);
  var S = window.__dbOSD;
  if (!S) return 'gone';  // JS context wiped (full page reload): the menu is truly gone
  var live = !!(S.root && S.root.isConnected);
  // close works even while detached, so an orphaned keep-alive node can always be swept.
  if (op === 'close') {
    S.close();
    return 'closed';
  }
  // 'detached' != 'gone': the node is momentarily off-DOM (a Leanback body swap) and the shared
  // keep-alive observer will re-append it. The launcher must not tear the menu down for that.
  if (op === 'state') return live ? S.state() : 'detached';
  if (op === 'cmd') {
    if (live) return S.exec(p.cmd);
    // Detached but still owned by the keep-alive: it WILL re-appear painted, so releasing capture
    // ('gone') would strand a visible menu with no owner (input passthrough). Swallow the key and
    // keep capture. Only a truly orphaned node (dropped from keep-alive, i.e. closed) reports 'gone'.
    return (window.__dbKeep && window.__dbKeep.indexOf(S.root) !== -1) ? 'consumed' : 'gone';
  }
  return 'gone';
})
