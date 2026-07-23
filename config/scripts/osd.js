// OSD Settings menu (osd-menu-plan.md). Invoked via p.op and returns a string over eval_string:
//   open  -> build from model params, "ok"        close -> tear down, "closed"
//   cmd   -> apply p.cmd, verdict "consumed"|"close"|"action:<id>"|"apply:<id>"|"gone"
//   state -> "tab=..;idx=.."
// Component lives on window.__dbOSD. CSP-safe (adoptedStyleSheets + CSSOM only, no style element or
// inline style attribute, no innerHTML) and keep-alive'd like the other launcher overlays.
//
// Settings has sub-tabs (Keys read-only, Captions writable). The Captions sub-tab is an editable
// surface: combo rows (←/→ cycle) and a dynamic preferred-language list (X removes an entry, ←/→
// reorders it by priority, A on the add-row opens a full-language picker). A combo/list edit returns
// "apply:cc.<key>=<val>" so the launcher persists it WITHOUT closing the menu. Held ↑/↓ auto-repeat
// (the launcher's own acceleration) for fast scanning; L1/R1 page-jump the picker ±10.
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
    "#__deckback_osd .panel{position:relative;display:none;flex:1 1 auto;flex-direction:column;" +
    "gap:14px;min-height:0;}" +
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
    "#__deckback_osd .keys{max-height:58vh;}" +
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
    "#__deckback_osd .subsel{display:flex;align-items:center;gap:14px;padding:8px 16px;" +
    "border-radius:10px;font-weight:600;flex:0 0 auto;}" +
    "#__deckback_osd .subsel .clbl{color:#9a9a9a;}" +
    "#__deckback_osd .subsel .arrow{color:#9fd0ff;font-weight:700;}" +
    "#__deckback_osd .cwrap{display:flex;flex-direction:column;gap:6px;flex:1 1 auto;min-height:0;}" +
    "#__deckback_osd .crow{display:flex;align-items:center;justify-content:space-between;gap:24px;" +
    "padding:12px 18px;border-radius:11px;font-size:23px;}" +
    "#__deckback_osd .crow .clbl{color:#9fd0ff;font-weight:600;}" +
    "#__deckback_osd .cval{color:#fff;display:flex;align-items:center;gap:12px;}" +
    "#__deckback_osd .cval .arrow{color:#9a9a9a;font-weight:700;}" +
    "#__deckback_osd .chead{color:#9fd0ff;font-weight:700;padding:8px 18px 0;}" +
    "#__deckback_osd .langrow{display:flex;align-items:center;justify-content:space-between;" +
    "gap:24px;padding:10px 18px 10px 34px;border-radius:11px;font-size:23px;}" +
    "#__deckback_osd .langrow .x{color:#9a9a9a;font-weight:600;font-size:19px;}" +
    "#__deckback_osd .addrow{padding:10px 18px 10px 34px;border-radius:11px;color:#9fd0ff;" +
    "font-weight:600;}" +
    "#__deckback_osd .picker{position:absolute;inset:0;z-index:5;background:rgba(6,6,6,0.985);" +
    "display:flex;flex-direction:column;gap:12px;padding:3vh 4vw;box-sizing:border-box;}" +
    "#__deckback_osd .ptitle{font-size:28px;font-weight:700;color:#fff;}" +
    "#__deckback_osd .plist{max-height:72vh;}" +
    "#__deckback_osd .pitem{padding:9px 22px;border-radius:10px;color:#eee;font-size:23px;}" +
    "#__deckback_osd .dbf{outline:3px solid #f5b301;outline-offset:2px;background:rgba(245,179,1,0.14);}" +
    "#__deckback_osd .hint{color:#9a9a9a;font-size:19px;border-top:2px solid rgba(255,255,255,0.12);" +
    "padding-top:10px;}" +
    "#__deckback_osd .hint .kb{font-weight:700;}";

  function ensureSheet() {
    try {
      if (!('adoptedStyleSheets' in document) || !window.CSSStyleSheet) return;
      if (!window.__dbOsdSheet) window.__dbOsdSheet = new CSSStyleSheet();
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

  function ensureVisible(box, node) {
    if (!box || !node) return;
    var top = node.offsetTop;
    var bot = top + node.offsetHeight;
    var vh = box.clientHeight || 0;
    if (top < box.__off) box.__off = top;
    else if (bot > box.__off + vh) box.__off = bot - vh;
    applyScroll(box);
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
    var rs = root.style;
    rs.setProperty('position', 'fixed');
    rs.setProperty('inset', '0');
    rs.setProperty('z-index', '2147483646');
    rs.setProperty('display', 'flex');
    rs.setProperty('flex-direction', 'column');
    rs.setProperty('background', 'rgba(8,8,8,0.95)');
    rs.setProperty('color', '#fff');

    var hasUpdate = !!p.upd_has;
    var cc = p.cc || null;

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

    var pSettings = el('div', 'panel');
    pSettings.setAttribute('data-tab', 'settings');
    var subs = ['keys'];
    if (cc) subs.push('captions');

    var subSel = el('div', 'subsel');
    subSel.setAttribute('data-focus', '1');
    subSel.setAttribute('data-role', 'subsel');
    subSel.appendChild(el('span', 'clbl', 'Section'));
    subSel.appendChild(el('span', 'arrow', subs.length > 1 ? '‹' : ''));
    var subLabel = el('span', null, 'Keys');
    subSel.appendChild(subLabel);
    subSel.appendChild(el('span', 'arrow', subs.length > 1 ? '›' : ''));
    pSettings.appendChild(subSel);

    var subContent = el('div', 'cwrap');
    pSettings.appendChild(subContent);

    var keysScroll = scrollBox('keys');
    var table = el('table');
    var kr = p.keys || [];
    for (var i = 0; i < kr.length; i++) {
      var tr = el('tr');
      tr.appendChild(el('td', 'k', kr[i][0]));
      tr.appendChild(el('td', 'v', kr[i][1]));
      table.appendChild(tr);
    }
    keysScroll.__inner.appendChild(table);

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
    var btns = p.upd_buttons || [];
    for (var j = 0; j < btns.length; j++) {
      var b = el('div', 'btn', btns[j][1]);
      b.setAttribute('data-focus', '1');
      b.setAttribute('data-action', btns[j][0]);
      actions.appendChild(b);
    }
    pUpdates.appendChild(actions);

    content.appendChild(pSettings);
    content.appendChild(pUpdates);

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
      var alinks = p.about_links || [];
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
    hint.appendChild(el('span', 'kb', 'X'));
    hint.appendChild(el('span', null, ' Remove   '));
    hint.appendChild(el('span', 'kb', 'L1/R1'));
    hint.appendChild(el('span', null, ' Tab   ←/→ Change · Reorder   ↑↓ Move'));

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
      focusIdx: -1,
      subs: subs,
      sub: 'keys',
      cc: cc,
      subLabel: subLabel,
      subContent: subContent,
      keysScroll: keysScroll,
      picker: null
    };

    function comboText(row) {
      var opts = row.options || [];
      for (var k = 0; k < opts.length; k++) if (opts[k].value === row.value) return opts[k].label;
      return row.value;
    }

    function langRow() {
      var rows = (S.cc && S.cc.rows) || [];
      for (var k = 0; k < rows.length; k++) if (rows[k].kind === 'langlist') return rows[k];
      return null;
    }

    function labelForCode(code) {
      var langs = (S.cc && S.cc.langs) || [];
      for (var k = 0; k < langs.length; k++) if (langs[k].value === code) return langs[k].label;
      return code;
    }

    function buildCaptions() {
      var rows = (S.cc && S.cc.rows) || [];
      for (var r = 0; r < rows.length; r++) {
        var row = rows[r];
        if (row.kind === 'combo') {
          var cr = el('div', 'crow');
          cr.setAttribute('data-focus', '1');
          cr.setAttribute('data-role', 'combo');
          cr.setAttribute('data-key', row.key);
          cr.appendChild(el('span', 'clbl', row.label));
          var val = el('div', 'cval');
          val.appendChild(el('span', 'arrow', '‹'));
          var vtext = el('span', null, comboText(row));
          val.appendChild(vtext);
          val.appendChild(el('span', 'arrow', '›'));
          cr.appendChild(val);
          cr.__row = row;
          cr.__vtext = vtext;
          S.subContent.appendChild(cr);
        } else {
          S.subContent.appendChild(el('div', 'chead', row.label));
          for (var it = 0; it < row.items.length; it++) {
            var item = row.items[it];
            var lr = el('div', 'langrow');
            lr.setAttribute('data-focus', '1');
            lr.setAttribute('data-role', 'langremove');
            lr.setAttribute('data-code', item.value);
            lr.appendChild(el('span', null, item.label));
            lr.appendChild(el('span', 'x', 'X: Remove'));
            S.subContent.appendChild(lr);
          }
          var add = el('div', 'addrow', '＋  Add language');
          add.setAttribute('data-focus', '1');
          add.setAttribute('data-role', 'add');
          S.subContent.appendChild(add);
        }
      }
    }

    function renderSub() {
      S.subLabel.textContent = S.sub === 'captions' ? 'Captions' : 'Keys';
      while (S.subContent.firstChild) S.subContent.removeChild(S.subContent.firstChild);
      if (S.sub === 'captions' && S.cc) buildCaptions();
      else S.subContent.appendChild(S.keysScroll);
    }

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
      if (S.tab === 'settings') renderSub();
      S.collect();
      if (typeof idx === 'number' && idx >= 0 && idx < S.focusables.length) S.focusIdx = idx;
      else S.focusIdx = S.focusables.length ? 0 : -1;
      S.applyRing();
      var box = S.panels[S.tab] ? S.panels[S.tab].querySelector('.scroll') : null;
      if (box) applyScroll(box);
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
    S.scroll = function (dir) {
      var panel = S.panels[S.tab];
      var box = panel ? panel.querySelector('.scroll') : null;
      if (!box) return 'consumed';
      var step = Math.max(48, Math.round((box.clientHeight || 200) * 0.2));
      box.__off = (box.__off || 0) + dir * step;
      applyScroll(box);
      return 'consumed';
    };

    function cycleSub(dir) {
      if (S.subs.length < 2) return 'consumed';
      var i = S.subs.indexOf(S.sub);
      S.sub = S.subs[(i + dir + S.subs.length) % S.subs.length];
      S.clearRing();
      renderSub();
      S.collect();
      S.focusIdx = 0;
      S.applyRing();
      return 'consumed';
    }
    function cycleCombo(node, dir) {
      var row = node.__row;
      var opts = row.options || [];
      if (!opts.length) return 'consumed';
      var i = 0;
      for (var k = 0; k < opts.length; k++) if (opts[k].value === row.value) i = k;
      i = (i + dir + opts.length) % opts.length;
      row.value = opts[i].value;
      node.__vtext.textContent = opts[i].label;
      return 'apply:cc.' + row.key + '=' + row.value;
    }
    function removeLang(node) {
      var code = node.getAttribute('data-code');
      var lr = langRow();
      if (!lr) return 'consumed';
      var kept = [];
      for (var k = 0; k < lr.items.length; k++) if (lr.items[k].value !== code) kept.push(lr.items[k]);
      if (!kept.length) kept.push({ value: 'system', label: labelForCode('system') });
      lr.items = kept;
      var keepIdx = S.focusIdx;
      S.clearRing();
      renderSub();
      S.collect();
      S.focusIdx = Math.min(keepIdx, S.focusables.length - 1);
      S.applyRing();
      return 'apply:cc.lang.remove=' + code;
    }
    function openPicker() {
      var pk = el('div', 'picker');
      pk.appendChild(el('div', 'ptitle', 'Add caption language'));
      var box = scrollBox('plist');
      var langs = (S.cc && S.cc.langs) || [];
      var els = [];
      for (var k = 0; k < langs.length; k++) {
        var pi = el('div', 'pitem', langs[k].label);
        pi.setAttribute('data-code', langs[k].value);
        box.__inner.appendChild(pi);
        els.push(pi);
      }
      pk.appendChild(box);
      var ph = el('div', 'hint');
      ph.appendChild(el('span', 'kb', 'A'));
      ph.appendChild(el('span', null, ' Add   '));
      ph.appendChild(el('span', 'kb', 'B'));
      ph.appendChild(el('span', null, ' Cancel   '));
      ph.appendChild(el('span', 'kb', 'L1/R1'));
      ph.appendChild(el('span', null, ' Jump   (hold ↑↓ to scan)'));
      pk.appendChild(ph);
      S.panels.settings.appendChild(pk);
      S.picker = { node: pk, box: box, els: els, idx: 0 };
      pickerFocus();
      return 'consumed';
    }
    function pickerFocus() {
      var pk = S.picker;
      for (var k = 0; k < pk.els.length; k++) pk.els[k].classList.toggle('dbf', k === pk.idx);
      ensureVisible(pk.box, pk.els[pk.idx]);
    }
    function pickerMove(delta) {
      var pk = S.picker;
      var n = pk.idx + delta;
      if (n < 0) n = 0;
      if (n > pk.els.length - 1) n = pk.els.length - 1;
      pk.idx = n;
      pickerFocus();
      return 'consumed';
    }
    function closePicker() {
      if (S.picker && S.picker.node) S.picker.node.remove();
      S.picker = null;
      S.applyRing();
    }
    function pickerSelect() {
      var pk = S.picker;
      var node = pk.els[pk.idx];
      var code = node ? node.getAttribute('data-code') : '';
      closePicker();
      if (!code) return 'consumed';
      var lr = langRow();
      var dup = false;
      if (lr) {
        for (var k = 0; k < lr.items.length; k++) if (lr.items[k].value === code) dup = true;
        if (!dup) lr.items.push({ value: code, label: labelForCode(code) });
      }
      S.clearRing();
      renderSub();
      S.collect();
      S.applyRing();
      return dup ? 'consumed' : 'apply:cc.lang.add=' + code;
    }
    function pickerExec(cmd) {
      switch (cmd) {
        case 'up': return pickerMove(-1);
        case 'down': return pickerMove(1);
        case 'tab_prev': return pickerMove(-10);
        case 'tab_next': return pickerMove(10);
        case 'scroll_up': return pickerMove(-1);
        case 'scroll_down': return pickerMove(1);
        case 'select': return pickerSelect();
        case 'back': closePicker(); return 'consumed';
      }
      return 'consumed';
    }

    S.activate = function () {
      var n = S.focusables[S.focusIdx];
      if (!n) return 'consumed';
      var role = n.getAttribute('data-role');
      if (role === 'add') return openPicker();
      var a = n.getAttribute('data-action');
      return a ? ('action:' + a) : 'consumed';
    };
    function moveOrScroll(dir) {
      if (S.tab === 'settings' && S.sub === 'keys') {
        var n = S.focusables[S.focusIdx];
        if (n && n.getAttribute('data-role') === 'subsel') return S.scroll(dir);
      }
      return S.focusables.length ? S.move(dir) : S.scroll(dir);
    }
    function edit(dir) {
      var n = S.focusables[S.focusIdx];
      if (!n) return 'consumed';
      var role = n.getAttribute('data-role');
      if (role === 'subsel') return cycleSub(dir);
      if (role === 'combo') return cycleCombo(n, dir);
      if (role === 'langremove') return moveLang(n, dir);
      return 'consumed';
    }
    function moveLang(node, dir) {
      var code = node.getAttribute('data-code');
      var lr = langRow();
      if (!lr) return 'consumed';
      var idx = -1;
      for (var k = 0; k < lr.items.length; k++) if (lr.items[k].value === code) idx = k;
      var j = idx + dir;
      if (idx < 0 || j < 0 || j >= lr.items.length) return 'consumed';
      var tmp = lr.items[idx];
      lr.items[idx] = lr.items[j];
      lr.items[j] = tmp;
      S.clearRing();
      renderSub();
      S.collect();
      for (var f = 0; f < S.focusables.length; f++)
        if (S.focusables[f].getAttribute('data-code') === code) { S.focusIdx = f; break; }
      S.applyRing();
      return 'apply:cc.lang.' + (dir < 0 ? 'up' : 'down') + '=' + code;
    }

    S.exec = function (cmd) {
      if (!S.root || !S.root.isConnected) return 'gone';
      if (S.picker) return pickerExec(cmd);
      switch (cmd) {
        case 'tab_next': S.cycleTab(1); return 'consumed';
        case 'tab_prev': S.cycleTab(-1); return 'consumed';
        case 'up': return moveOrScroll(-1);
        case 'down': return moveOrScroll(1);
        case 'left': return edit(-1);
        case 'right': return edit(1);
        case 'select': return S.activate();
        case 'delete': {
          var d = S.focusables[S.focusIdx];
          if (d && d.getAttribute('data-role') === 'langremove') return removeLang(d);
          return 'consumed';
        }
        case 'ignore':
          if (S.tab === 'updates' && S.panels.updates.querySelector('[data-action="update.ignore"]'))
            return 'action:update.ignore';
          return 'consumed';
        case 'back': return 'close';
        case 'scroll_up': return S.scroll(-1);
        case 'scroll_down': return S.scroll(1);
      }
      return 'consumed';
    };
    S.state = function () {
      if (!S.root || !S.root.isConnected) return 'gone';
      return 'tab=' + S.tab + ';idx=' + S.focusIdx + ';sub=' + S.sub + ';pick=' + (S.picker ? 1 : 0);
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
  if (!S) return 'gone';
  var live = !!(S.root && S.root.isConnected);
  if (op === 'close') {
    S.close();
    return 'closed';
  }
  if (op === 'state') return live ? S.state() : 'detached';
  if (op === 'cmd') {
    if (live) return S.exec(p.cmd);
    return (window.__dbKeep && window.__dbKeep.indexOf(S.root) !== -1) ? 'consumed' : 'gone';
  }
  return 'gone';
})
