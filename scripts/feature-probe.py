#!/usr/bin/env python3
"""P12.0 — the probes that classify a mechanism before any feature is built on it.

Runs on the WORKSTATION, drives the running app on the Deck over SSH + CDP (read-only):

    player    P12.0b  what does the TVHTML5 player expose for playback RATE and QUALITY?
    pairing   P12.0a  is Leanback's own "Link with TV code" pairing reachable under our Cobalt UA?
    dock      P12.0c  what does gamescope do to our fixed 1280x800 surface when a TV is attached?
                      (snapshot undocked and docked, then diff)

Exit codes (.internal/HARNESS.md §1):

    0  the probe ran and produced an answer -- a NEGATIVE answer is still 0
    2  ASSERT     an invariant of the probe itself broke
    3  ENV        cannot observe: app not on Leanback, no player, nothing was navigated to
    4  TRANSPORT  the Deck or its DevTools endpoint went away
    5  USAGE
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_REPO / "scripts"))
sys.path.insert(0, str(_REPO / "tests" / "deck"))

import cdp as cdplib  # noqa: E402
from lib import probes, ssh as sshlib  # noqa: E402

EX_OK, EX_FAIL, EX_ASSERT, EX_ENV, EX_TRANSPORT, EX_USAGE = 0, 1, 2, 3, 4, 5

# Not `#movie_player`: the TVHTML5 app does not use the desktop id.
PLAYER_SELECTOR = ".html5-video-player"

RATE_PATTERN = re.compile(r"rate|speed|tempo", re.I)
QUALITY_PATTERN = re.compile(r"quality|resolution|format|itag|level|bitrate|abr", re.I)

# Names the desktop/iframe player is documented to carry; always called explicitly, never assumed.
KNOWN_RATE_METHODS = (
    "getPlaybackRate",
    "setPlaybackRate",
    "getAvailablePlaybackRates",
)
KNOWN_QUALITY_METHODS = (
    "getPlaybackQuality",
    "setPlaybackQuality",
    "setPlaybackQualityRange",
    "getAvailableQualityLevels",
    "getAvailableQualityData",
    "getPreferredQuality",
)

# Every function-valued property along the prototype chain (the player's methods live on its
# prototype), plus what the <video> itself reports.
PLAYER_DUMP_EXPR = """(function(){
  var el = document.querySelector('%s');
  if (!el) return JSON.stringify({found: false});
  var names = {}, o = el;
  while (o && o !== Object.prototype) {
    var own = Object.getOwnPropertyNames(o);
    for (var i = 0; i < own.length; i++) {
      var k = own[i];
      try { if (typeof el[k] === 'function') names[k] = 1; } catch (e) {}
    }
    o = Object.getPrototypeOf(o);
  }
  var v = document.querySelector('video');
  return JSON.stringify({
    found: true,
    methods: Object.keys(names).sort(),
    video: v ? {
      playbackRate: v.playbackRate,
      defaultPlaybackRate: v.defaultPlaybackRate,
      paused: v.paused,
      currentTime: v.currentTime,
      duration: v.duration,
      width: v.videoWidth,
      height: v.videoHeight
    } : null
  });
})()""" % PLAYER_SELECTOR


def _call_expr(method_names):
    """Call each named method with no arguments, capturing value-or-throw for each. Never changes
    playback: a setter that needs an argument just throws."""
    return """(function(){
  var el = document.querySelector('%s');
  if (!el) return JSON.stringify({found: false});
  var out = {};
  var names = %s;
  for (var i = 0; i < names.length; i++) {
    var n = names[i];
    if (typeof el[n] !== 'function') { out[n] = {exists: false}; continue; }
    try { out[n] = {exists: true, value: el[n]()}; }
    catch (e) { out[n] = {exists: true, threw: String(e && e.message || e)}; }
  }
  return JSON.stringify({found: true, results: out});
})()""" % (PLAYER_SELECTOR, json.dumps(list(method_names)))


def rate_write_expr(target):
    """Set `<video>.playbackRate` and read it straight back. The caller re-reads later for a snap-back."""
    return """(function(){
  var v = document.querySelector('video');
  if (!v) return JSON.stringify({video: false});
  var before = v.playbackRate;
  var threw = null;
  try { v.playbackRate = %s; } catch (e) { threw = String(e && e.message || e); }
  return JSON.stringify({video: true, before: before, after: v.playbackRate, threw: threw});
})()""" % json.dumps(target)


RATE_READ_EXPR = (
    "(function(){var v=document.querySelector('video');"
    "return v?v.playbackRate:-1;})()"
)


def restore_rate_expr(value):
    return """(function(){
  var v = document.querySelector('video');
  if (v) { try { v.playbackRate = %s; } catch (e) {} }
  return v ? v.playbackRate : -1;
})()""" % json.dumps(value)


# The TV app's own config, where a lounge/pairing-capable build advertises itself. Read-only.
PAIRING_CONFIG_EXPR = """(function(){
  var keys = [], cfgs = [];
  try { if (window.ytcfg && ytcfg.data_) cfgs.push(ytcfg.data_); } catch (e) {}
  try { if (window.yt && yt.config_) cfgs.push(yt.config_); } catch (e) {}
  for (var i = 0; i < cfgs.length; i++) {
    for (var k in cfgs[i]) {
      if (/LOUNGE|PAIR|MDX|SCREEN|REMOTE|CAST/i.test(k)) keys.push(k);
    }
  }
  var uniq = {}, out = [];
  for (var j = 0; j < keys.length; j++) if (!uniq[keys[j]]) { uniq[keys[j]] = 1; out.push(keys[j]); }
  return JSON.stringify({keys: out.sort(), hasCfg: cfgs.length > 0});
})()"""

# What is on screen right now: innerText plus aria-label/title/alt, since Leanback labels much of its
# chrome on icon nodes that innerText never sees.
SCREEN_TEXT_EXPR = """(function(){
  var t = (document.body && document.body.innerText) || '';
  var labels = [];
  try {
    var nodes = document.querySelectorAll('[aria-label],[title],[alt]');
    for (var i = 0; i < nodes.length && labels.length < 400; i++) {
      var n = nodes[i];
      var v = n.getAttribute('aria-label') || n.getAttribute('title') || n.getAttribute('alt');
      if (v) labels.push(v);
    }
  } catch (e) {}
  var joined = labels.join('\\n');
  return JSON.stringify({
    len: t.length, text: t.slice(0, 4000),
    labelCount: labels.length, labels: joined.slice(0, 4000),
    hash: location.hash
  });
})()"""

PAIRING_PATTERN = re.compile(
    r"link\s+with\s+tv\s+code|tv\s+code|pair(ing)?\s+(with|code)|enter\s+(the\s+)?code|"
    r"youtube\.com/(pair|tv/activate)|link\s+device",
    re.I,
)


# ---- pure helpers (unit-tested in tests/harness/test_feature_probe.py) ----


def classify_methods(names):
    """Bucket a method dump into {'rate', 'quality', 'other'} by name pattern. A name matching both
    patterns lands in both buckets."""
    rate = sorted(n for n in names if RATE_PATTERN.search(n))
    quality = sorted(n for n in names if QUALITY_PATTERN.search(n))
    interesting = set(rate) | set(quality)
    return {
        "rate": rate,
        "quality": quality,
        "other": sorted(n for n in names if n not in interesting),
    }


def video_is_loaded(video):
    """Is there real media in the element, or just an idle player shell (as on the browse screen,
    where every quality getter answers `[]`)?"""
    if not video:
        return False
    duration = video.get("duration")
    return bool(duration) or bool(video.get("width")) or bool(video.get("currentTime"))


def player_verdict(dump, calls, rate_test):
    """(exit_code, headline, lines) for P12.0b. No player on the page (`found` false) is ENV, never
    a negative."""
    if not dump.get("found"):
        return (
            EX_ENV,
            f"INCONCLUSIVE — no `{PLAYER_SELECTOR}` on the page.",
            [
                "Nothing was observed, so this run says nothing about the player's API.",
                "Open a video on the Deck (the browse screen carries a player only sometimes) and",
                "re-run. A method list read off a page with no player is not a negative result.",
            ],
        )

    loaded = video_is_loaded(dump.get("video"))
    methods = dump.get("methods") or []
    buckets = classify_methods(methods)
    lines = []

    def _worked(name):
        r = (calls or {}).get(name) or {}
        return bool(r.get("exists")) and "value" in r

    rate_getters = [n for n in KNOWN_RATE_METHODS if _worked(n)]
    quality_getters = [n for n in KNOWN_QUALITY_METHODS if _worked(n)]
    named_rate = [n for n in KNOWN_RATE_METHODS if ((calls or {}).get(n) or {}).get("exists")]
    named_quality = [n for n in KNOWN_QUALITY_METHODS if ((calls or {}).get(n) or {}).get("exists")]

    # `<video>.playbackRate` needs no player API, but counts only if the player lets the value stand.
    rt = rate_test or {}
    held = rt.get("held")
    if rt.get("video") and held is True and not loaded:
        # The player reapplies its rate only while loading a stream, so an idle hold proves nothing.
        lines.append(f"<video>.playbackRate held at {rt.get('after')}, but on an element with NO")
        lines.append("media loaded — the player had nothing to reassert. This does NOT establish")
        lines.append("that a rate survives playback. Re-run with a video actually playing.")
        held = None
    elif rt.get("video") and held is True:
        lines.append(f"<video>.playbackRate held at {rt.get('after')} for {rt.get('settle_s')}s of")
        lines.append("real playback — playback speed is buildable on the element alone.")
    elif rt.get("video") and held is False:
        lines.append(
            f"<video>.playbackRate was set to {rt.get('after')} and snapped back to "
            f"{rt.get('later')} within {rt.get('settle_s')}s — the player overrides it."
        )
        lines.append("Speed must go through a player API (if any) or be re-asserted continuously.")
    elif rt.get("video") is False:
        lines.append("No <video> element was present for the rate test.")

    if named_rate:
        lines.append(f"Player rate methods present: {', '.join(named_rate)}"
                     + (f" (answered: {', '.join(rate_getters)})" if rate_getters else " (none answered)"))
    else:
        lines.append("No player method from the known rate set exists on this build.")
    if named_quality:
        lines.append(f"Player quality methods present: {', '.join(named_quality)}"
                     + (f" (answered: {', '.join(quality_getters)})" if quality_getters else " (none answered)"))
    else:
        lines.append("No player method from the known quality set exists on this build.")

    if not loaded:
        lines.append("")
        lines.append("CAVEAT: no media was loaded (duration/videoWidth are empty), so every quality")
        lines.append("getter answered [] or \"unknown\" because there is nothing to describe — NOT")
        lines.append("because the API is absent. The level list is only meaningful during playback.")

    if buckets["rate"] or buckets["quality"]:
        lines.append("")
        lines.append("Name-matched candidates worth calling by hand before designing anything:")
        if buckets["rate"]:
            lines.append(f"  rate     {', '.join(buckets['rate'])}")
        if buckets["quality"]:
            lines.append(f"  quality  {', '.join(buckets['quality'])}")

    can_rate = bool(named_rate) or held is True
    can_quality = bool(named_quality) or bool(buckets["quality"])
    if can_rate and can_quality:
        head = "RATE and QUALITY are both reachable — P12.5 and the P12.1 quality drop are unblocked."
    elif can_rate:
        head = "RATE is reachable, QUALITY is not — P12.5 unblocked; P12.1 drops video by other means."
    elif can_quality:
        head = "QUALITY is reachable, RATE is not — P12.9/P12.1 unblocked; P12.5 needs another lever."
    else:
        head = "NEITHER rate nor quality is reachable on this build — register it and stop."
    return (EX_OK, head, lines)


def pairing_hits(text):
    """The distinct pairing-ish phrases found in on-screen text, in order of appearance."""
    if not text:
        return []
    seen, out = set(), []
    for m in PAIRING_PATTERN.finditer(text):
        phrase = " ".join(m.group(0).split()).lower()
        if phrase not in seen:
            seen.add(phrase)
            out.append(phrase)
    return out


# Below this a capture is "thin" (the watch screen carries ~28 chars) and does not count as a look.
MIN_SCREEN_TEXT = 80


def screen_is_substantive(text, minimum=MIN_SCREEN_TEXT):
    """Did this capture actually see a screen with content, or a bare player?"""
    return len((text or "").strip()) >= minimum


def is_watch_screen(hash_):
    """`#/watch?v=...` in any of the forms location.hash reports it."""
    return (hash_ or "").lstrip("#/").lower().startswith("watch")


# Rows that appear on Leanback's settings screen. Two distinct markers are required: the browse
# screen's sidebar carries a lone "Settings".
SETTINGS_MARKERS = (
    re.compile(r"linked\s+devices?", re.I),
    re.compile(r"restricted\s+mode", re.I),
    re.compile(r"\bprivacy\b", re.I),
    re.compile(r"\bsign\s+out\b", re.I),
    re.compile(r"\baccessibility\b", re.I),
    re.compile(r"\bnotifications?\b", re.I),
    re.compile(r"\blanguage\b", re.I),
    re.compile(r"\blocation\b", re.I),
    re.compile(r"\babout\b", re.I),
    re.compile(r"\bsettings\b", re.I),
)
MIN_SETTINGS_MARKERS = 2


def settings_markers(text):
    """Which settings-screen markers this capture carries."""
    return [m.pattern for m in SETTINGS_MARKERS if m.search(text or "")]


def could_host_pairing(screen):
    """Could a 'Link with TV code' entry appear here? Needs content, not the watch screen, and at
    least MIN_SETTINGS_MARKERS settings markers."""
    text = screen.get("text")
    return (
        screen_is_substantive(text)
        and not is_watch_screen(screen.get("hash"))
        and len(settings_markers(text)) >= MIN_SETTINGS_MARKERS
    )


def pairing_verdict(screens, config_keys):
    """(exit_code, headline, lines) for P12.0a.

    `screens` is [{'label', 'text', 'hash'}, ...] (`text` = innerText + aria-labels). With no
    screen that could host the entry this is ENV; zero hits on one that could is the negative.
    """
    lookable = [s for s in screens if could_host_pairing(s)]

    # Hits count from any screen; only the negative needs a lookable one.
    found = [(s.get("label"), h) for s, h in ((s, pairing_hits(s.get("text"))) for s in screens) if h]
    if not lookable and not found:
        seen = ", ".join(
            f"{s.get('label')}@{s.get('hash') or '?'}"
            f"{' [thin]' if not screen_is_substantive(s.get('text')) else ''}"
            for s in screens
        )
        return (
            EX_ENV,
            "INCONCLUSIVE — nothing was captured on a screen that could show a pairing entry.",
            [
                f"captured: {seen or '(nothing)'}",
                "settings markers seen: "
                + str({s.get("label"): settings_markers(s.get("text")) for s in screens}),
                "Neither the player nor the browse feed is such a screen. The player has no menu,",
                "and the feed is long, text-rich and carries a sidebar — so 'plenty of text, not the",
                f"player' was not enough. A negative needs >={MIN_SETTINGS_MARKERS} settings markers",
                "on the captured screen. Open Leanback's own Settings and re-run.",
            ],
        )

    lines = [
        "screens that could have shown it: "
        + (", ".join(s.get("label") or "?" for s in lookable) or "(none)")
    ]
    if config_keys:
        lines.append(f"lounge/pairing keys in the TV app config: {', '.join(config_keys)}")
    else:
        lines.append("no lounge/pairing keys in the TV app config (weak signal on its own).")

    if found:
        for label, h in found:
            lines.append(f"  {label}: {', '.join(h)}")
        return (
            EX_OK,
            "PAIRING IS REACHABLE under our UA — P12.3 is a docs/SUPPORT.md section, not code.",
            lines
            + [
                "",
                "Confirm by hand that a code actually renders and a phone can claim it, then write",
                "the SUPPORT.md section. Do not build a remote (feature-landscape §2.3).",
            ],
        )
    return (
        EX_OK,
        "NO PAIRING ENTRY on the screens captured — register a page-layer dead end for P12.3.",
        lines
        + [
            "",
            "Same shape as the voice-search lesson (input-ux §13.2): our own UA is the most likely",
            "thing suppressing it. Before registering, make sure the Settings screen itself was one",
            "of the screens captured above — a miss on the wrong screen is not a negative.",
        ],
    )


# ---- P12.0c: docked output ----

# The page's view of its own surface: changes if gamescope resizes us, not if it upscales.
VIEWPORT_EXPR = """JSON.stringify({
  innerWidth: window.innerWidth,
  innerHeight: window.innerHeight,
  dpr: window.devicePixelRatio,
  screenWidth: screen.width,
  screenHeight: screen.height,
  video: (function(){
    var v = document.querySelector('video');
    return v ? {w: v.videoWidth, h: v.videoHeight,
                cw: Math.round(v.getBoundingClientRect().width),
                ch: Math.round(v.getBoundingClientRect().height)} : null;
  })()
})"""

# What is physically plugged in: the control for a "nothing changed" diff.
DRM_STATUS_CMD = "grep -H . /sys/class/drm/card*/status 2>/dev/null"

# Each connector's preferred mode, in the same `<path>:<value>` shape (parse_drm_connectors reads both).
DRM_MODES_CMD = (
    'for f in /sys/class/drm/card*/modes; do echo "$f:$(head -1 "$f" 2>/dev/null)"; done 2>/dev/null'
)

# The Deck's built-in panel. Everything else is an external output.
INTERNAL_CONNECTOR = re.compile(r"eDP|LVDS|DSI", re.I)


def parse_drm_connectors(text):
    """{connector name: status} from `grep -H . /sys/class/drm/card*/status`."""
    out = {}
    for line in (text or "").splitlines():
        path, _, status = line.partition(":")
        if not status:
            continue
        name = path.strip().split("/")
        if len(name) < 2:
            continue
        # .../card1-HDMI-A-1/status -> card1-HDMI-A-1, then drop the card prefix.
        connector = name[-2]
        connector = connector.split("-", 1)[1] if "-" in connector else connector
        out[connector] = status.strip()
    return out


def external_outputs(connectors):
    """Connected outputs that are not the built-in panel."""
    return sorted(
        name for name, status in (connectors or {}).items()
        if status == "connected" and not INTERNAL_CONNECTOR.search(name)
    )


def dock_verdict(before, after):
    """(exit_code, headline, lines) for P12.0c. ENV unless an external output appeared between the
    two snapshots."""
    gained = sorted(set(external_outputs(after.get("drm"))) - set(external_outputs(before.get("drm"))))
    if not gained:
        present = external_outputs(after.get("drm"))
        return (
            EX_ENV,
            "INCONCLUSIVE — no external output appeared between the two snapshots.",
            [
                f"external outputs before: {external_outputs(before.get('drm')) or '(none)'}",
                f"external outputs after:  {present or '(none)'}",
                "Nothing was plugged in (or the connector never reported `connected`), so any",
                "'nothing changed' below is about an undocked Deck and says nothing about docking.",
            ],
        )

    pb, pa = before.get("page") or {}, after.get("page") or {}
    gb, ga = before.get("geometry"), after.get("geometry")
    modes = after.get("modes") or {}
    lines = [
        f"external output(s) gained: {', '.join(gained)}"
        + (f" advertising {', '.join(modes.get(g, '?') for g in gained)}" if modes else ""),
        f"gamescope display geometry: {gb} -> {ga}",
        f"page innerWidth/Height:     [{pb.get('innerWidth')}, {pb.get('innerHeight')}]"
        f" -> [{pa.get('innerWidth')}, {pa.get('innerHeight')}]",
        f"screen.width/height:        [{pb.get('screenWidth')}, {pb.get('screenHeight')}]"
        f" -> [{pa.get('screenWidth')}, {pa.get('screenHeight')}]",
        f"devicePixelRatio:           {pb.get('dpr')} -> {pa.get('dpr')}",
        f"decoded video:              {pb.get('video')} -> {pa.get('video')}",
    ]

    surface_changed = (
        pa.get("innerWidth") != pb.get("innerWidth")
        or pa.get("innerHeight") != pb.get("innerHeight")
    )
    display_changed = ga != gb
    if surface_changed:
        return (
            EX_OK,
            "OUR SURFACE RESIZES when docked — P12.2 is a resize path, not a relaunch.",
            lines
            + [
                "",
                "The page sees the new size, so Leanback relayouts itself and the open question",
                "becomes whether --content-shell-host-window-size still pins anything and whether",
                "the engine renders at the new size or just stretches. Measure decoded video size",
                "against the panel before claiming 1080p output.",
            ],
        )
    if display_changed:
        return (
            EX_OK,
            "GAMESCOPE RESIZES, WE DO NOT — P12.2 needs a relaunch at the new mode.",
            lines
            + [
                "",
                "The compositor's display changed while our surface stayed put, i.e. an 800p buffer",
                "is being upscaled to the TV. The watchdog restart machinery already exists; the",
                "design question is detecting the mode change and choosing when to take the restart.",
            ],
        )
    external_mode = ", ".join(modes.get(g, "") for g in gained).strip(", ")
    tail = [
        "",
        "An external output is connected, enabled, and nothing above it moved. The compositor's own",
        "screen never grew, so there is no larger surface for us to resize INTO: this is not a",
        "window-management problem we can solve from the app, and neither a runtime resize nor a",
        "relaunch would find more pixels than gamescope is offering.",
    ]
    if external_mode and ga and external_mode.split("x")[0] not in (ga or "").split():
        tail += [
            f"The panel advertises {external_mode} while gamescope's screen is {ga} — the external",
            "mode never reached the nested Xwayland at all.",
        ]
    tail += [
        "",
        "P12.2 is therefore a SESSION/COMPOSITOR question (how gamescope is started and how SteamOS",
        "switches outputs when docked), not an app one — consistent with platform.md's finding that",
        "the QAM knobs are user-facing settings and not flags we can pass. Confirm what the TV is",
        "physically showing before registering: black, mirrored-and-upscaled, and 'nothing at all'",
        "are three different findings and this reading cannot tell them apart.",
    ]
    return (
        EX_OK,
        "DOCKING CHANGES NOTHING WE CAN SEE — neither our surface nor gamescope's geometry moved.",
        lines + tail,
    )


# ---- driver ----


def _say(msg=""):
    print(msg, file=sys.stderr, flush=True)


def _ask(prompt):
    _say("")
    _say(f">>> {prompt}")
    _say(">>> press Enter here when done (Ctrl-C aborts)")
    try:
        sys.stdin.readline()
    except KeyboardInterrupt:
        raise SystemExit(EX_OK)


def _report(code, head, lines):
    _say("")
    _say("=" * 72)
    _say(head)
    _say("=" * 72)
    for ln in lines:
        _say(ln)
    return code


def cmd_player(cdp, args, sh=None, disp=None):
    raw = cdp.evaluate(PLAYER_DUMP_EXPR)
    if raw is None:
        raise ConnectionError("the page did not answer the dump (engine restarted?)")
    dump = json.loads(raw)
    if not dump.get("found"):
        return _report(*player_verdict(dump, {}, {}))

    methods = dump.get("methods") or []
    buckets = classify_methods(methods)
    _say(f"player found: {len(methods)} function-valued properties along the prototype chain")
    _say(f"  video      {dump.get('video')}")
    _say(f"  rate-ish   {buckets['rate'] or '(none)'}")
    _say(f"  quality-ish{'':1} {buckets['quality'] or '(none)'}")

    probe_names = sorted(set(KNOWN_RATE_METHODS + KNOWN_QUALITY_METHODS)
                         | set(buckets["rate"]) | set(buckets["quality"]))
    calls = json.loads(cdp.evaluate(_call_expr(probe_names)) or "{}").get("results", {})
    _say("")
    _say("--- calling each candidate with no arguments " + "-" * 27)
    for name in probe_names:
        r = calls.get(name) or {}
        if not r.get("exists"):
            continue
        if "value" in r:
            _say(f"    {name:<28} -> {json.dumps(r['value'])[:120]}")
        else:
            _say(f"    {name:<28} !! {r.get('threw')}")

    rate_test = {}
    if not args.no_rate_write:
        _say("")
        _say(f"--- <video>.playbackRate write test (target {args.rate}) " + "-" * 12)
        rt = json.loads(cdp.evaluate(rate_write_expr(args.rate)) or "{}")
        rate_test = dict(rt)
        if rt.get("video"):
            _say(f"    before {rt.get('before')}  after {rt.get('after')}  threw {rt.get('threw')}")
            import time as _t

            _t.sleep(args.settle)
            later = cdp.evaluate(RATE_READ_EXPR)
            rate_test["later"] = later
            rate_test["settle_s"] = args.settle
            rate_test["held"] = later is not None and abs(float(later) - float(args.rate)) < 1e-6
            _say(f"    after {args.settle}s: {later}  -> {'HELD' if rate_test['held'] else 'RESET'}")
            back = cdp.evaluate(restore_rate_expr(rt.get("before") or 1.0))
            _say(f"    restored to {back}")
        else:
            _say("    no <video> element — skipped")

    return _report(*player_verdict(dump, calls, rate_test))


def cmd_pairing(cdp, args, sh=None, disp=None):
    cfg = json.loads(cdp.evaluate(PAIRING_CONFIG_EXPR) or "{}")
    keys = cfg.get("keys") or []
    _say(f"TV app config present: {cfg.get('hasCfg')}")
    _say(f"lounge/pairing-ish config keys: {keys or '(none)'}")

    screens = []

    def capture(label):
        raw = cdp.evaluate(SCREEN_TEXT_EXPR)
        if raw is None:
            raise ConnectionError("the page stopped answering")
        got = json.loads(raw)
        text = "\n".join(x for x in (got.get("text", ""), got.get("labels", "")) if x)
        screen = {"label": label, "text": text, "hash": got.get("hash")}
        screens.append(screen)
        hits = pairing_hits(text)
        mark = "" if could_host_pairing(screen) else "  [cannot host a pairing entry — not a look]"
        _say(f"    {label}: at {got.get('hash')!r}, {got.get('len')} chars innerText + "
             f"{got.get('labelCount')} aria-labels, pairing hits: {hits or '(none)'}{mark}")
        return text

    _say("")
    _say("--- screens " + "-" * 60)
    capture("start")
    for label, prompt in (
        ("settings", "On the Deck, open Leanback's own Settings screen (not our OSD menu) "
                     "and leave it on screen."),
        ("link-with-tv-code", "If you can see a 'Link with TV code' / pairing entry, open it. "
                              "If there is none, just press Enter."),
    ):
        if args.unattended:
            break
        _ask(prompt)
        capture(label)

    return _report(*pairing_verdict(screens, keys))


def find_display(sh):
    """The Xwayland display where OUR content_shell is focused, or None.

    SteamOS runs `--xwayland-count 2`: Steam is on :0, a Steam-launched game on :1.
    """
    for disp in (":0", ":1", ":2"):
        _, cls, _ = sh.run(
            f"DISPLAY={disp} xdotool getwindowfocus getwindowclassname 2>/dev/null", check=False
        )
        if "content_shell" in (cls or "").lower():
            return disp
    return None


def cmd_dock(cdp, args, sh=None, disp=None):
    def snapshot(label):
        _, drm, _ = sh.run(DRM_STATUS_CMD, check=False)
        _, raw_modes, _ = sh.run(DRM_MODES_CMD, check=False)
        _, geom, _ = sh.run(f"DISPLAY={disp} xdotool getdisplaygeometry 2>/dev/null", check=False)
        _, xr, _ = sh.run(f"DISPLAY={disp} xrandr --current 2>/dev/null | head -2", check=False)
        page = json.loads(cdp.evaluate(VIEWPORT_EXPR) or "{}")
        connectors = parse_drm_connectors(drm)
        snap = {
            "drm": connectors,
            "modes": {k: v for k, v in parse_drm_connectors(raw_modes).items() if v},
            "geometry": (geom or "").strip() or None,
            "xrandr": (xr or "").strip(),
            "page": page,
        }
        _say(f"    {label}: outputs={external_outputs(connectors) or '(none external)'} "
             f"geometry={snap['geometry']!r} "
             f"page=[{page.get('innerWidth')}x{page.get('innerHeight')}] dpr={page.get('dpr')}")
        if snap["modes"]:
            _say(f"      connector modes (preferred): {snap['modes']}")
        return snap

    _say("")
    _say("--- snapshots " + "-" * 58)

    # --save takes one snapshot and stops; --against takes the second and compares; else interactive.
    if args.save:
        snap = snapshot("snapshot")
        Path(args.save).write_text(json.dumps(snap, indent=2))
        _say(f"wrote {args.save} — re-run with --against {args.save} once the state has changed.")
        return EX_OK

    if args.against:
        before = json.loads(Path(args.against).read_text())
        _say(f"    undocked (from {args.against}): "
             f"outputs={external_outputs(before.get('drm')) or '(none external)'} "
             f"geometry={before.get('geometry')!r}")
        after = snapshot("docked")
        return _report(*dock_verdict(before, after))

    before = snapshot("undocked")
    _ask("Attach the dock / HDMI adapter to the Deck now and let the TV come up, then press Enter.")
    after = snapshot("docked")
    return _report(*dock_verdict(before, after))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="command", required=True)

    p = sub.add_parser("player", help="P12.0b — what the TVHTML5 player exposes for rate/quality")
    p.add_argument("--rate", type=float, default=1.25, help="playbackRate to try (default 1.25)")
    p.add_argument("--settle", type=float, default=3.0,
                   help="seconds to wait before re-reading the rate (default 3)")
    p.add_argument("--no-rate-write", action="store_true",
                   help="dump only; never touch <video>.playbackRate")
    p.set_defaults(func=cmd_player)

    p = sub.add_parser("pairing", help="P12.0a — is 'Link with TV code' reachable under our UA")
    p.add_argument("--unattended", action="store_true",
                   help="capture only the current screen (no human prompts); usually INCONCLUSIVE")
    p.set_defaults(func=cmd_pairing)

    p = sub.add_parser("dock", help="P12.0c — what gamescope does to our surface when docked")
    p.add_argument("--save", metavar="FILE",
                   help="take one snapshot, write it, and stop (run this UNDOCKED)")
    p.add_argument("--against", metavar="FILE",
                   help="compare a fresh snapshot against a saved one (run this DOCKED)")
    p.set_defaults(func=cmd_dock)

    for p in (ap,):
        p.add_argument("--deck-host", default=None, help="override DECK_HOST")
    args = ap.parse_args(argv)

    host = args.deck_host or sshlib.deck_host()
    port = sshlib.deck_port()
    if not host or not sshlib.reachable(host, port):
        _say(f"error: no Deck reachable at {host or '<unset DECK_HOST>'}:{port}")
        return EX_TRANSPORT

    sh = sshlib.Ssh(host, port)
    try:
        with sshlib.Tunnel(host, port, 9222) as tun:
            with cdplib.CDP(tun.cdp_port) as cdp:
                if not cdp.wait_for(probes.ON_LEANBACK_EXPR, timeout=45):
                    _say("error: the engine is not on youtube.com/tv (is the app running?)")
                    return EX_ENV
                disp = find_display(sh)
                if args.command == "dock" and not disp:
                    _say("error: our content_shell is not the focused window on :0, :1 or :2, so")
                    _say("       any display geometry read back would be Steam's, not ours.")
                    return EX_ENV
                return args.func(cdp, args, sh, disp)
    except sshlib.NoDevTools as e:
        _say(f"error: {e}")
        return EX_ENV
    except (sshlib.DeckUnreachable, ConnectionError, OSError) as e:
        _say(f"error: transport failed — {e}")
        return EX_TRANSPORT


if __name__ == "__main__":
    sys.exit(main())
