#!/usr/bin/env python3
"""Minimal, dependency-free Chrome DevTools Protocol client.

Two lives:
  * a **library** (`from cdp import CDP, key_spec, MediaState`) for the on-Deck pytest harness
    (TEST-PLAN §3) and the conformance driver (§6);
  * a **CLI** for the smoke gate (`scripts/smoke.sh`), unchanged.

Stdlib only — no `websocket-client`, no Selenium, no Playwright. They violate the minimal-deps rule
and cannot run inside the build image (TEST-PLAN §8).

Two protocol details that are load-bearing, both learned the hard way:
  * Chrome >= 111 requires `Host: localhost` on the `/json` endpoints, so we always talk to
    127.0.0.1 and you must TUNNEL (`ssh -L 9222:localhost:9222`) rather than hit the Deck's LAN IP.
  * Leanback tears the page target down on navigation. `CDP.call` transparently re-attaches and
    re-arms the enabled domains + the sticky UA override; without that the client dies mid-suite.

Exception vocabulary matches the retry policy in TEST-PLAN §0: `ConnectionError`/`TimeoutError` are
transport and may be retried; `RuntimeError` is a CDP-level error and must NOT be.
"""
import argparse
import base64
import json
import os
import socket
import struct
import sys
import time
import urllib.request

# Exceptions a caller may retry. Everything else means the product (or our expression) is wrong.
RETRYABLE = (ConnectionError, TimeoutError)

# CDP Input.dispatchKeyEvent `modifiers` bitfield. Protocol-defined; not ours to choose.
MOD_ALT, MOD_CTRL, MOD_META, MOD_SHIFT = 1, 2, 4, 8

# The codec strings YouTube TV actually probes. `av01.0.05M.08` is Main profile, level 5.0, 8-bit —
# the format Leanback requests for 1080p. `OK_MIMES` is the negative control for the AV1 steering:
# a script that reported everything unsupported would pass an AV1-only check and serve nothing.
AV1_MIME = 'video/mp4; codecs="av01.0.05M.08"'
OK_MIMES = ('video/webm; codecs="vp9"', 'video/mp4; codecs="avc1.640028"')

# Named (non-printable) keys -> windowsVirtualKeyCode. Mirrors launcher/src/devtools.cpp:kNamedKeys;
# the media VKs are tree-verified in cobalt/src/starboard/key.h (kSbKeyMediaRewind = 0xE3 = 227).
NAMED_KEYS = {
    "ArrowUp": 38, "ArrowDown": 40, "ArrowLeft": 37, "ArrowRight": 39,
    "Enter": 13, "Escape": 27, "Backspace": 8, "Delete": 46, "Tab": 9,
    "MediaPlayPause": 179, "MediaTrackNext": 176, "MediaTrackPrevious": 177,
    "MediaStop": 178, "MediaRewind": 227, "MediaFastForward": 228,
}
KEY_ALIAS = {"Up": "ArrowUp", "Down": "ArrowDown", "Left": "ArrowLeft", "Right": "ArrowRight",
             "Return": "Enter", "Esc": "Escape", "Back": "Backspace"}
# US-layout DOM `code` + VK for printable non-alphanumerics. A shifted char carries the code/VK of the
# physical key that produces it ('!' lives on Digit1) — the DOM contract; the caller adds MOD_SHIFT.
_PUNCT = {
    " ": ("Space", 32), "-": ("Minus", 189), "_": ("Minus", 189), "=": ("Equal", 187),
    "+": ("Equal", 187), "[": ("BracketLeft", 219), "{": ("BracketLeft", 219),
    "]": ("BracketRight", 221), "}": ("BracketRight", 221), "\\": ("Backslash", 220),
    "|": ("Backslash", 220), ";": ("Semicolon", 186), ":": ("Semicolon", 186),
    "'": ("Quote", 222), '"': ("Quote", 222), ",": ("Comma", 188), "<": ("Comma", 188),
    ".": ("Period", 190), ">": ("Period", 190), "/": ("Slash", 191), "?": ("Slash", 191),
    "`": ("Backquote", 192), "~": ("Backquote", 192), "!": ("Digit1", 49), "@": ("Digit2", 50),
    "#": ("Digit3", 51), "$": ("Digit4", 52), "%": ("Digit5", 53), "^": ("Digit6", 54),
    "&": ("Digit7", 55), "*": ("Digit8", 56), "(": ("Digit9", 57), ")": ("Digit0", 48),
}

# Back-compat for callers that imported the old table.
KEYMAP = {name: (name, name, vk) for name, vk in NAMED_KEYS.items()}

# Compact description of the focused element — the observable signal that navigation moved.
# TEST-PLAN §0: assert that *this* changed, never that we dispatched the key we chose to dispatch.
FOCUS_EXPR = (
    "(function(){var e=document.activeElement;if(!e)return 'none';"
    "var t=e.tagName.toLowerCase();var c=(e.className||'').toString().trim().split(/\\s+/)[0]||'';"
    "var a=(e.getAttribute&&(e.getAttribute('aria-label')||e.getAttribute('aria-selected')||''))||'';"
    "return t+(c?'.'+c:'')+(a?' <'+a+'>':'');})()"
)


def key_spec(name):
    """Resolve a key name to (key, code, vk, text) or None if we cannot synthesize it.

    `name` is a named key ("ArrowUp", "MediaRewind") or a single printable ASCII character.
    `text` is non-empty only for printable keys: Blink needs it on a `keyDown` (not `rawKeyDown`) to
    produce a char event, which is what makes beforeinput/textInput fire and a character land in a
    focused <input>. Returning None rather than guessing is the point — see the no-guessing policy.
    """
    name = KEY_ALIAS.get(name, name)
    if name in NAMED_KEYS:
        return (name, name, NAMED_KEYS[name], "")
    if len(name) != 1:
        return None
    c = name
    if "a" <= c <= "z":
        return (c, "Key" + c.upper(), ord(c.upper()), c)
    if "A" <= c <= "Z":
        return (c, "Key" + c, ord(c), c)
    if "0" <= c <= "9":
        return (c, "Digit" + c, ord(c), c)
    if c in _PUNCT:
        code, vk = _PUNCT[c]
        return (c, code, vk, c)
    return None


class MediaState:
    """Accumulates CDP `Media` domain events into per-player property maps.

    Pure: `feed()` takes decoded CDP messages, so it is unit-testable with no socket. This is the
    supported way to learn the decoder identity (TEST-PLAN §5) — do NOT scrape chrome://media-internals,
    which is an unstable WebUI over the same MediaLog stream.

    This class ACCUMULATES and does not ADJUDICATE. The P4 hardware-decode verdict lives in exactly
    one place, `tests/deck/lib/probes.py:hardware_decode_verdict`; a second copy here drifted from it
    within a week (it read a missing `kIsPlatformVideoDecoder` as "not a platform decoder", turning a
    broken probe into a product failure).
    """

    def __init__(self):
        self.players = {}  # playerId -> {property name: value}

    def feed(self, msg):
        if msg.get("method") != "Media.playerPropertiesChanged":
            return
        p = msg.get("params", {})
        pid = p.get("playerId")
        if pid is None:
            return
        props = self.players.setdefault(pid, {})
        for prop in p.get("properties", []):
            name, value = prop.get("name"), prop.get("value")
            # Cache the latest NON-null value: the engine re-emits properties with null while
            # tearing a player down, and a null would erase a decoder name we already observed.
            if name is not None and value is not None:
                props[name] = value

    def latest(self, name):
        """The most recently seen value of `name` across all players, or None."""
        for props in reversed(list(self.players.values())):
            if name in props:
                return props[name]
        return None

    @property
    def decoder_name(self):
        return self.latest("kVideoDecoderName")

    @property
    def is_platform_decoder(self):
        v = self.latest("kIsPlatformVideoDecoder")
        return None if v is None else str(v).lower() == "true"


def http_json(port, path):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=5) as r:
        return json.load(r)


def wait_endpoint(port, tries=40):
    for _ in range(tries):
        try:
            return http_json(port, "/json/version")
        except Exception:
            time.sleep(1)
    raise ConnectionError(f"DevTools endpoint on :{port} never came up")


def pick_page_target(port, tries=30):
    for _ in range(tries):
        try:
            for t in http_json(port, "/json"):
                if t.get("type") == "page" and t.get("webSocketDebuggerUrl"):
                    return t["webSocketDebuggerUrl"]
        except Exception:
            pass
        time.sleep(1)
    raise ConnectionError("no page target with a ws url appeared")


def _ws_url(port, target_id):
    """Build the page ws URL ourselves.

    `/json/new` echoes the `Host:` header back in `webSocketDebuggerUrl`, so it can come back as
    `ws://localhost/devtools/page/<id>` with no port at all, which WS() would dial on :80.
    """
    return f"ws://127.0.0.1:{port}/devtools/page/{target_id}"


def new_page_target(port, url="about:blank"):
    """Create a page target of our own and return (target_id, ws_url).

    Chrome >= 111 requires PUT here, and requires a Host header of localhost or a literal IP.
    """
    req = urllib.request.Request(
        f"http://127.0.0.1:{port}/json/new?{url}", method="PUT")
    with urllib.request.urlopen(req, timeout=10) as r:
        t = json.load(r)
    tid = t["id"]
    return tid, _ws_url(port, tid)


def close_target(port, target_id):
    """Best-effort: a leaked about:blank window outlives the run and confuses the next one."""
    try:
        with urllib.request.urlopen(
                f"http://127.0.0.1:{port}/json/close/{target_id}", timeout=5):
            pass
    except Exception:
        pass


def target_exists(port, target_id):
    try:
        return any(t.get("id") == target_id for t in http_json(port, "/json"))
    except Exception:
        return False


class WS:
    """Just enough RFC6455 client for CDP: masked text frames out, reassembled frames in."""

    def __init__(self, url):
        assert url.startswith("ws://")
        host_port, _, self.path = url[5:].partition("/")
        self.path = "/" + self.path
        host, _, port = host_port.partition(":")
        self.sock = socket.create_connection((host, int(port or 80)), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {self.path} HTTP/1.1\r\nHost: {host_port}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += self.sock.recv(4096)
        if b" 101 " not in buf.split(b"\r\n", 1)[0]:
            raise ConnectionError(f"ws handshake failed: {buf[:120]!r}")
        self._rest = buf.split(b"\r\n\r\n", 1)[1]

    def settimeout(self, seconds):
        self.sock.settimeout(seconds)

    def _recv(self, n):
        while len(self._rest) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("ws closed")
            self._rest += chunk
        out, self._rest = self._rest[:n], self._rest[n:]
        return out

    def send(self, text):
        data = text.encode()
        hdr = bytearray([0x81])  # FIN + text
        n = len(data)
        if n < 126:
            hdr.append(0x80 | n)
        elif n < 65536:
            hdr.append(0x80 | 126)
            hdr += struct.pack(">H", n)
        else:
            hdr.append(0x80 | 127)
            hdr += struct.pack(">Q", n)
        mask = os.urandom(4)
        hdr += mask
        self.sock.sendall(bytes(hdr) + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def recv(self):
        payload = b""
        while True:
            b0, b1 = self._recv(2)
            fin, opcode = b0 & 0x80, b0 & 0x0F
            length = b1 & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._recv(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._recv(8))[0]
            payload += self._recv(length)
            if opcode == 0x8:  # close
                raise ConnectionError("ws close frame")
            if fin:
                return payload.decode(errors="replace")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class CDP:
    """A CDP client bound to one page target.

    `own_target=True` creates a dedicated page instead of attaching to whatever is already open.
    That is not a tidiness preference: `deckback-launcher` runs a Navigator that watches its own page
    and re-navigates it back to youtube.com/tv whenever it leaves. Anything that drives the shared
    target -- `cert.py --deck` above all -- is dragged home mid-run and reports that its page "failed
    to load". The launcher's navigation policy is not what the conformance suite is testing.
    """

    def __init__(self, port, own_target=False):
        self.port = port
        self._enabled = []
        self._ua = None
        self._own_target = own_target
        self._target_id = None
        self.media = MediaState()
        self.connect()

    # Usable as `with CDP(9222) as cdp:` — pytest fixtures need a close(), the old client had none.
    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        if getattr(self, "ws", None):
            self.ws.close()
            self.ws = None
        if self._own_target and self._target_id:
            close_target(self.port, self._target_id)
            self._target_id = None

    def connect(self):
        if self._own_target:
            # Re-attach after a target teardown must land on a page the launcher still does not own,
            # so mint a fresh one rather than falling back to pick_page_target().
            if not self._target_id or not target_exists(self.port, self._target_id):
                self._target_id, url = new_page_target(self.port)
            else:
                url = _ws_url(self.port, self._target_id)
        else:
            url = pick_page_target(self.port)
        self.ws = WS(url)
        self._id = 0
        for method in self._enabled:  # re-arm domains on a fresh target
            self.call(method, _no_reconnect=True)
        if self._ua:  # Leanback tears down targets; keep the UA sticky
            self.call("Network.enable", _no_reconnect=True)
            self.call("Network.setUserAgentOverride", {"userAgent": self._ua}, _no_reconnect=True)

    def set_user_agent(self, ua):
        """Sticky UA override, re-armed on every re-attach. content_shell ignores --user-agent."""
        self._ua = ua
        self.call("Network.enable")
        self.call("Network.setUserAgentOverride", {"userAgent": ua})

    def call(self, method, params=None, timeout=30, _no_reconnect=False):
        if method.endswith(".enable") and method not in self._enabled:
            self._enabled.append(method)
        try:
            self._id += 1
            mid = self._id
            self.ws.send(json.dumps({"id": mid, "method": method, "params": params or {}}))
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                msg = json.loads(self.ws.recv())
                if msg.get("id") == mid:
                    if "error" in msg:
                        raise RuntimeError(f"{method}: {msg['error']}")
                    return msg.get("result", {})
                # Not our reply: it is an event. Feed it to the Media accumulator instead of
                # discarding it — Media.playerPropertiesChanged arrives unsolicited between calls,
                # and dropping it is why the decoder identity was previously unobtainable.
                self.media.feed(msg)
            raise TimeoutError(f"{method} timed out")
        except (ConnectionError, BrokenPipeError, OSError):
            if _no_reconnect:
                raise
            self.connect()  # target was torn down (Leanback navigations) — re-attach
            return self.call(method, params, timeout, _no_reconnect=True)

    def pump(self, seconds):
        """Read unsolicited events for `seconds`, feeding MediaState. Returns the events seen."""
        seen = []
        deadline = time.monotonic() + seconds
        self.ws.settimeout(0.25)
        try:
            while time.monotonic() < deadline:
                try:
                    msg = json.loads(self.ws.recv())
                except (socket.timeout, TimeoutError):
                    continue
                except ConnectionError:
                    self.connect()
                    self.ws.settimeout(0.25)
                    continue
                if "method" in msg:
                    self.media.feed(msg)
                    seen.append(msg)
        finally:
            self.ws.settimeout(10)
        return seen

    def evaluate(self, expr):
        r = self.call("Runtime.evaluate",
                      {"expression": expr, "returnByValue": True, "awaitPromise": True})
        return r.get("result", {}).get("value")

    # ---- input ----------------------------------------------------------------------------------

    def dispatch_key(self, name, modifiers=0):
        """Synthesize a trusted key press. CDP-injected keys have isTrusted=true (S0.6)."""
        spec = key_spec(name)
        if spec is None:
            raise ValueError(f"unknown key {name!r} (named: {', '.join(NAMED_KEYS)}; or one printable char)")
        key, code, vk, text = spec
        self.dispatch_raw(key, code, vk, text=text, modifiers=modifiers)

    def dispatch_raw(self, key, code, vk, text="", modifiers=0):
        """Dispatch an arbitrary key by explicit VK.

        Needed for the §4 spikes' NEGATIVE CONTROLS: inject codes we believe inert (CEA 461/415,
        which belong to Tizen/webOS, not Chromium) and require they do nothing. Without a negative
        control, "the key worked" is unfalsifiable.
        """
        common = {"key": key, "code": code, "windowsVirtualKeyCode": vk, "nativeVirtualKeyCode": vk}
        if modifiers:
            common["modifiers"] = modifiers
        down = dict(common)
        if text:
            down["text"] = text
        self.call("Input.dispatchKeyEvent", {"type": "keyDown" if text else "rawKeyDown", **down})
        self.call("Input.dispatchKeyEvent", {"type": "keyUp", **common})

    def type_text(self, s, settle_ms=0):
        """Type a string one trusted keystroke at a time (not Input.insertText).

        The question text entry actually asks is whether Leanback's field responds to real key events;
        insertText would bypass exactly the code path under test.
        """
        for ch in s:
            mods = MOD_SHIFT if (ch.isupper() or ch in '~!@#$%^&*()_+{}|:"<>?') else 0
            self.dispatch_key(ch, modifiers=mods)
            if settle_ms:
                time.sleep(settle_ms / 1000.0)

    def mouse(self, kind, x, y, button="left", buttons=1, click_count=1):
        self.call("Input.dispatchMouseEvent", {
            "type": kind, "x": x, "y": y, "button": button,
            "buttons": buttons, "clickCount": click_count,
        })

    def click(self, x, y):
        """Trusted synthetic click. Coordinates are CSS pixels, so getBoundingClientRect() feeds
        straight in — no letterbox transform (unlike raw touch evdev coords). This is how voice
        search must be activated: kSbKeyMicrophone does nothing on this build (findings §13.2)."""
        self.mouse("mousePressed", x, y, buttons=1)
        self.mouse("mouseReleased", x, y, buttons=0)

    # ---- probes ---------------------------------------------------------------------------------

    def enable_media(self):
        """Subscribe to the CDP Media domain — the supported source of decoder identity (§5)."""
        self.call("Media.enable")

    def screenshot(self, path, timeout=30):
        """Best-effort: headless SwiftShader can stall Page.captureScreenshot. A paint failure must
        not fail a run whose gate is a DOM assertion. Returns True when a file was written."""
        try:
            shot = self.call("Page.captureScreenshot", {"format": "png"}, timeout=timeout)
            with open(path, "wb") as f:
                f.write(base64.b64decode(shot["data"]))
            return True
        except Exception:
            return False

    def screenshot_png(self, timeout=30):
        """Return the current page as raw PNG bytes, or None on a paint failure. Same best-effort
        contract as screenshot(): a stalled capture must not raise into a test's gate."""
        try:
            shot = self.call("Page.captureScreenshot", {"format": "png"}, timeout=timeout)
            return base64.b64decode(shot["data"])
        except Exception:
            return None

    def wait_for(self, expr, timeout=45, interval=1.0):
        """Poll a JS predicate until truthy. Returns True/False; never raises on a CDP error."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                if self.evaluate(expr):
                    return True
            except Exception:
                pass
            time.sleep(interval)
        return False


class AssertionFailure(Exception):
    """A `--expect`/`--assert`/`--baseline` predicate was not truthy. Exit 2, never retried."""


def run_assertions(evaluate, exprs, label="assert"):
    """Evaluate each expression and require it to be exactly `True`.

    Truthiness is deliberately NOT enough. A JS expression that returns the string "false", or the
    number 0, or `undefined` because we typo'd a property name, is a broken check — and a broken
    check that passes is worse than no check (finding F1). Every predicate must say `=== true`.

    `evaluate` is injected so this is unit-testable with no browser. Returns the list of
    `(expr, value)` pairs that failed, empty on success.
    """
    failures = []
    for expr in exprs:
        value = evaluate(expr)
        if value is True:
            print(f"{label} ok: {expr}")
        else:
            print(f"{label} FAILED: {expr}\n  -> {value!r}", file=sys.stderr)
            failures.append((expr, value))
    return failures


# The T6 capability probes, as they are actually spelled on the wire. Kept here rather than in
# smoke.sh because bash quoting of nested JS strings is where these go to die.
def codec_assertions(av1_mime, ok_mimes):
    """AV1 must look unsupported through all three APIs YouTube probes; the others must not.

    The `ok_mimes` half is a mandatory negative control. Steering that reported *everything*
    unsupported would satisfy an AV1-only assertion perfectly, while YouTube served nothing at all.
    """
    def q(m):
        return m.replace("\\", "\\\\").replace("'", "\\'")

    out = [
        f"MediaSource.isTypeSupported('{q(av1_mime)}') === false",
        f"document.createElement('video').canPlayType('{q(av1_mime)}') === ''",
        ("(async function(){var r=await navigator.mediaCapabilities.decodingInfo("
         "{type:'media-source',video:{contentType:'%s',width:1280,height:720,bitrate:2000000,"
         "framerate:30}});return r.supported === false;})()" % q(av1_mime)),
    ]
    for m in ok_mimes:
        out.append(f"MediaSource.isTypeSupported('{q(m)}') === true")
        out.append(f"document.createElement('video').canPlayType('{q(m)}') !== ''")
    return out


def av1_baseline_assertion(av1_mime):
    """The control for the control: BEFORE the steering script is injected, the engine must say AV1
    *is* supported.

    Without this, a Cobalt build compiled with no AV1 decoder at all would make every steering
    assertion pass while proving precisely nothing — the same shape as `just power` scoring a perfect
    0.00 W from a battery that reports no telemetry.
    """
    q = av1_mime.replace("\\", "\\\\").replace("'", "\\'")
    return f"MediaSource.isTypeSupported('{q}') === true"


def _run(args):
    """The CLI body. Returns the process exit code (see EXIT CODES in main())."""
    ver = wait_endpoint(args.port)
    print(f"browser UA (pre-override): {ver.get('User-Agent')}")

    cdp = CDP(args.port)
    cdp.call("Page.enable")
    cdp.call("Runtime.enable")
    if args.ua:
        cdp.set_user_agent(args.ua)
        print(f"UA override applied: {args.ua}")

    # Baselines run on the current (about:blank) document, BEFORE any script is injected. They
    # establish that the thing we are about to suppress was there to suppress.
    baselines = list(args.baseline or [])
    if args.assert_av1_steering:
        baselines.append(av1_baseline_assertion(AV1_MIME))
    if baselines and run_assertions(cdp.evaluate, baselines, label="baseline"):
        raise AssertionFailure(
            "a baseline check failed. The engine's UNSTEERED behaviour is not what we assumed, so "
            "the assertions that follow would pass vacuously."
        )

    # Same mechanism the launcher uses (navigator.cpp), so smoke exercises the shipped script.
    for path in args.add_script or []:
        with open(path) as f:
            cdp.call("Page.addScriptToEvaluateOnNewDocument", {"source": f.read()})
        print(f"injected on new document: {path}")

    if args.navigate:
        cdp.call("Page.navigate", {"url": args.navigate})

    ready = cdp.wait_for(args.expect, timeout=args.ready_timeout)

    title = cdp.evaluate("document.title")
    url = cdp.evaluate("document.location.href")
    ua_seen = cdp.evaluate("navigator.userAgent")
    body_len = cdp.evaluate("document.body ? document.body.innerText.length : -1")
    print(f"ready={ready} title={title!r} url={url!r} body_chars={body_len}")
    print(f"navigator.userAgent={ua_seen}")
    if args.print_expr:
        print(f"probe={cdp.evaluate(args.print_expr)}")

    def screenshot(path):
        if cdp.screenshot(path):
            print(f"screenshot: {path} ({os.path.getsize(path)} bytes)")
        else:
            print(f"screenshot skipped ({path})")

    if args.out:
        screenshot(args.out)

    if not ready:
        # Run no capability probes against a page that never loaded: every one of them would fail,
        # and the pile of red would bury the single real cause.
        return 2

    checks = list(args.assert_expr or [])
    if args.assert_ua:
        if not args.ua:
            raise ValueError("--assert-ua requires --ua")
        # The page's view of the UA, compared against what we handed to Network.setUserAgentOverride.
        # They differ exactly when the sticky override was lost across a navigation — the R1 failure.
        checks.append(f"navigator.userAgent === {json.dumps(args.ua)}")
    if args.assert_av1_steering:
        checks += codec_assertions(AV1_MIME, OK_MIMES)
    if checks and run_assertions(cdp.evaluate, checks):
        raise AssertionFailure(f"{len(checks)} check(s) ran; see the failures above")

    if args.keys:
        print(f"focus[start]={cdp.evaluate(FOCUS_EXPR)!r}")
        base, ext = os.path.splitext(args.out) if args.out else (None, None)
        for i, k in enumerate(x.strip() for x in args.keys.split(",") if x.strip()):
            cdp.dispatch_key(k)
            time.sleep(args.key_settle / 1000.0)
            print(f"key[{i:02d}] {k:>10} -> focus={cdp.evaluate(FOCUS_EXPR)!r} "
                  f"url={cdp.evaluate('location.href')!r}")
            if args.out:
                screenshot(f"{base}.{i:02d}{ext}")

    cdp.close()
    return 0 if ready else 2


def main():
    """EXIT CODES (the taxonomy in .internal/HARNESS.md §1 — an unattended runner depends on them):

      0  the readiness assertion held
      2  ASSERTION failed: the engine is up but the page never satisfied --expect. A regression.
      4  TRANSPORT: no DevTools endpoint, socket died, target never appeared. Retryable.
      1  anything else (a CDP-level error, a bad expression) — investigate, do not retry blindly.

    Previously any transport failure escaped as an uncaught traceback and exit 1, indistinguishable
    from a real assertion failure. `just smoke` is a CI gate; conflating "the Deck was asleep" with
    "Leanback broke" is how a gate gets ignored.
    """
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9222)
    ap.add_argument("--ua")
    ap.add_argument("--navigate")
    ap.add_argument("--out")
    ap.add_argument("--expect", default="!!document.body && document.body.innerText.length >= 0")
    ap.add_argument("--print", dest="print_expr")
    ap.add_argument("--ready-timeout", type=int, default=45)
    ap.add_argument("--add-script", action="append", metavar="FILE",
                    help="Page.addScriptToEvaluateOnNewDocument, before --navigate (repeatable)")
    ap.add_argument("--baseline", action="append", metavar="EXPR",
                    help="must be === true BEFORE --add-script (repeatable). A negative control.")
    ap.add_argument("--assert", dest="assert_expr", action="append", metavar="EXPR",
                    help="must be === true after the page is ready (repeatable)")
    ap.add_argument("--assert-ua", action="store_true",
                    help="assert navigator.userAgent still equals --ua (the R1 canary)")
    ap.add_argument("--assert-av1-steering", action="store_true",
                    help="T6 capability probes: AV1 unsupported via all three APIs, VP9/H.264 not")
    # S0.6 input spike: dispatch a comma-separated key sequence (e.g. "Down,Down,Right,Enter") after
    # the page is ready, logging the focused element after each so we can see navigation move. With
    # --out, writes a numbered screenshot per key (<out>.NN.png).
    ap.add_argument("--keys")
    ap.add_argument("--key-settle", type=int, default=700, help="ms to wait after each key")
    args = ap.parse_args()

    try:
        sys.exit(_run(args))
    except AssertionFailure as e:
        print(f"assertion failure: {e}", file=sys.stderr)
        sys.exit(2)
    except RETRYABLE as e:
        print(f"transport failure: {type(e).__name__}: {e}", file=sys.stderr)
        sys.exit(4)
    except ValueError as e:  # e.g. an unknown key name passed to --keys
        print(f"usage error: {e}", file=sys.stderr)
        sys.exit(5)
    except RuntimeError as e:  # a CDP-level error: our expression or the protocol, not the wire
        print(f"cdp error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
