#!/usr/bin/env python3
"""L3 conformance: run the self-hosted `js_mse_eme` suites and adjudicate against a baseline.

TEST-PLAN §6. Two tiers, one script:

    scripts/cert.py --suite conformance-test              # workstation, Xvfb + content_shell
    scripts/cert.py --suite conformance-test --deck       # real hardware, over SSH + reverse tunnel

**Passing this suite is NOT YouTube certification** and must never be described as such
(`docs/legal.md`). It is a regression net built from the harness Google used to certify devices.
That is a strong claim on its own and does not need inflating.

Why self-hosted at all: the public runners are gone (verified 2026-07-09). `ytlr-cert.appspot.com`
and `yts.devicecertification.youtube` serve a sunset stub and the `yt-dash-mse-test` unit-test URLs
404. `github.com/youtube/js_mse_eme` is archived read-only but live, and the media vectors still
serve from GCS. We pin the harness (`tests/cert/HARNESS.pin`) and mirror the vectors.

Serving from `localhost` is load-bearing twice over, not a convenience:

  * **EME needs a secure context.** `requestMediaKeySystemAccess` is gated on a potentially-trustworthy
    origin. `http://localhost` qualifies; `http://192.168.x.y:8000` does not. Serve over a LAN IP and
    every EME test fails at the first call, for a reason that has nothing to do with our engine.
  * **It silences the phone-home.** The harness POSTs results to `qual-e.appspot.com` when the page
    URL contains `appspot.com`/`googleapis.com` (`harness/test.js`). A localhost origin never does.

Both are asserted before a single test runs, so a misconfiguration fails loudly instead of
masquerading as a product bug.

Exit codes (.internal/HARNESS.md §1):

    0  no regressions
    2  ASSERT     a test that used to pass does not, or the suite never finished
    3  ENV        no baseline, no harness, vector hash mismatch, insecure origin, no engine
    4  TRANSPORT  the Deck or the DevTools endpoint went away
    5  USAGE
"""

from __future__ import annotations

import argparse
import contextlib
import functools
import hashlib
import http.server
import json
import os
import socket
import socketserver
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_REPO / "scripts"))
sys.path.insert(0, str(_REPO / "scripts" / "lib"))
sys.path.insert(0, str(_REPO / "tests" / "deck"))

import cdp as cdplib  # noqa: E402
import cert_harness as ch  # noqa: E402
from lib import ssh as sshlib  # noqa: E402

from exitcodes import (EX_OK, EX_FAIL, EX_ASSERT, EX_ENV, EX_TRANSPORT,  # noqa: E402
                       EX_USAGE, Parser as _Parser)

HARNESS_DIR = _REPO / ".cache" / "js_mse_eme"
VECTOR_DIR = _REPO / ".cache" / "cert-vectors"
PIN_FILE = _REPO / "tests" / "cert" / "HARNESS.pin"

# `/echo` exists to reflect eight bytes (ch.ECHO_ROUTE). Nothing legitimate posts more.
ECHO_MAX_BYTES = 64 * 1024
EXPECT_DIR = _REPO / "tests" / "cert" / "expectations"
VECTOR_MANIFEST = _REPO / "tests" / "cert" / "vectors.sha256"
RESULT_DIR = _REPO / "artifacts" / "cert"

# TEST-PLAN §6.3. `playbackperf-*` is a trend, not a gate: its results depend on TDP, refresh rate,
# and thermals, so on a handheld it is not deterministic. Recorded, never used to fail a build.
GATE_SUITES = ("conformance-test", "msecodec-test", "encryptedmedia-test")
TREND_SUITES = ("playbackperf-sfr-vp9-test", "playbackperf-sfr-h264-test",
                "playbackperf-sfr-av1-test", "playbackperf-hfr-test")


def _say(msg):
    print(msg, file=sys.stderr, flush=True)


def _rel(path):
    """Repo-relative if we can, absolute otherwise.

    `Path.relative_to` RAISES when the path is outside the repo, so building an error message with it
    turned a clean exit 3 ("no baseline") into a ValueError traceback and exit 1 — the error handler
    failing louder than the error.
    """
    try:
        return Path(path).relative_to(_REPO)
    except ValueError:
        return Path(path)


class Env(Exception):
    """Something about the setup is wrong. Exit 3, and never mistake it for a decoder regression."""


# ---- vendoring the harness -----------------------------------------------------------------------

class Pin:
    __slots__ = ("name", "url", "sha", "subdir")

    def __init__(self, name, url, sha, subdir):
        self.name, self.url, self.sha, self.subdir = name, url, sha, subdir

    @property
    def path(self):
        return HARNESS_DIR if self.subdir == "." else HARNESS_DIR / self.subdir


def read_pins():
    """Parse `<name> <url> <sha> <subdir>` records. The harness record must come first — the others
    are checked out *inside* its tree, so it has to exist before they can land."""
    pins = []
    for line in PIN_FILE.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != 4:
            raise Env(f"{PIN_FILE}: expected 'name url sha subdir', got: {line!r}")
        name, url, sha, subdir = parts
        if len(sha) != 40 or not all(c in "0123456789abcdef" for c in sha):
            raise Env(f"{PIN_FILE}: {name} pin is not a 40-char sha: {sha!r}")
        if subdir != "." and (subdir.startswith("/") or ".." in subdir.split("/")):
            raise Env(f"{PIN_FILE}: {name} subdir must be relative and free of '..': {subdir!r}")
        pins.append(Pin(name, url, sha, subdir))
    if not pins or pins[0].name != "harness":
        raise Env(f"{PIN_FILE}: the first record must be named 'harness'")
    return pins


def _fetch_pin(pin):
    """Fetch exactly the pinned commit into `pin.path`. Shallow: we want a tree, not a history."""
    dest = pin.path
    if (dest / ".git").exists():
        got = subprocess.run(["git", "-C", str(dest), "rev-parse", "HEAD"],
                             capture_output=True, text=True).stdout.strip()
        if got == pin.sha:
            return
        _say(f"{pin.name} at {got[:12]}, want {pin.sha[:12]} — refetching")

    dest.mkdir(parents=True, exist_ok=True)

    def git(*a):
        r = subprocess.run(["git", "-C", str(dest), *a], capture_output=True, text=True)
        if r.returncode != 0:
            raise Env(f"git {' '.join(a)} failed in {pin.name}: {r.stderr.strip()}")

    if not (dest / ".git").exists():
        git("init", "-q")
        git("remote", "add", "origin", pin.url)
    # `fetch --depth 1 <sha>` pulls that one commit; a branch clone would drift under the pin.
    git("fetch", "-q", "--depth", "1", "origin", pin.sha)
    git("checkout", "-q", "-f", "FETCH_HEAD")
    _say(f"{pin.name}: {pin.url} @ {pin.sha[:12]}")


def sync_harness(pins):
    """Vendor the harness AND the two trees it forgets to ship.

    `main.html` lists 86 script sources; 44 of them are under `third_party/` and upstream provides
    none of them — no submodule, no `.gitmodules`, no fetch script. Without Closure and protobuf's
    JS runtime the page renders blank, `window.globalRunner` is never created, and `cert.py` reports
    "the harness never installed window.globalRunner". Nothing about the source says so.
    """
    for pin in pins:
        _fetch_pin(pin)
    missing = missing_script_sources(HARNESS_DIR)
    if missing:
        raise Env(
            f"{len(missing)} of main.html's script sources are missing after sync, e.g. "
            f"{missing[0]}. A pinned dependency in {PIN_FILE.name} is wrong or incomplete; the "
            "harness would load to a blank page and never create window.globalRunner."
        )
    return HARNESS_DIR


def missing_script_sources(root):
    """Which of `main.html`'s `scriptSources` do not exist on disk?

    Run after every sync. The failure it prevents is silent and expensive: the harness page loads,
    44 script tags 404, nothing throws where we can see it, and the only symptom 60 seconds later is
    "window.globalRunner is missing" — which reads like an engine bug.
    """
    import re

    html = (root / "main.html").read_text()
    m = re.search(r"var scriptSources = \[(.*?)\];", html, re.S)
    if not m:
        raise Env("main.html has no `scriptSources` array; the harness changed shape")
    return [p for p in re.findall(r'"([^"]+)"', m.group(1)) if not (root / p).exists()]


# ---- the vector mirror ---------------------------------------------------------------------------
#
# The harness fetches ~244 media files from a third-party bucket whose sibling endpoints Google has
# already sunset. We rewrite MEDIA_PATH/CERT_PATH to our own origin and serve from a local cache that
# is filled on first miss and SHA-256 verified thereafter.
#
# A hash mismatch exits 3, not 2. "Google changed the bucket" and "our decoder broke" are different
# sentences, and at 3 a.m. only one of them is worth waking up for.

def load_manifest():
    if not VECTOR_MANIFEST.exists():
        return {}
    out = {}
    for line in VECTOR_MANIFEST.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        digest, _, name = line.partition("  ")
        out[name.strip()] = digest.strip()
    return out


def save_manifest(entries):
    VECTOR_MANIFEST.parent.mkdir(parents=True, exist_ok=True)
    body = ["# SHA-256 of every media/cert vector `just cert` has served, one per line.",
            "# Recorded by `scripts/cert.py --freeze-vectors`. A mismatch on a later run exits 3",
            "# (the upstream bucket changed), never 2 (our decoder broke).",
            "#",
            "# We fetch and verify these; we never redistribute them. Same posture as CdmFetcher.",
            ""]
    body += [f"{d}  {n}" for n, d in sorted(entries.items())]
    VECTOR_MANIFEST.write_text("\n".join(body) + "\n")


def fetch_vector(kind, rel, manifest, record):
    """Return the bytes of `<kind>/<rel>`, from cache or the bucket, hash-checked either way."""
    key = f"{kind}/{rel}"
    path = VECTOR_DIR / kind / rel
    if path.exists():
        blob = path.read_bytes()
    else:
        base = ch.REMOTE_MEDIA_BASE if kind == "media" else ch.REMOTE_CERT_BASE
        url = base + rel
        try:
            with urllib.request.urlopen(url, timeout=60) as r:
                blob = r.read()
        except urllib.error.HTTPError as e:
            raise Env(f"vector {key} is gone from the bucket (HTTP {e.code}). Google moved it; "
                      "this is not a decoder regression.") from e
        except OSError as e:
            raise Env(f"cannot fetch vector {key}: {e}") from e
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(blob)

    digest = hashlib.sha256(blob).hexdigest()
    known = manifest.get(key)
    if known is None:
        record[key] = digest
    elif known != digest:
        raise Env(f"vector {key} does not match tests/cert/vectors.sha256 "
                  f"(expected {known[:16]}…, got {digest[:16]}…). The upstream bucket changed. "
                  "Delete .cache/cert-vectors and re-freeze after reviewing the diff.")
    return blob


# ---- the server ----------------------------------------------------------------------------------

class Handler(http.server.SimpleHTTPRequestHandler):
    """Serves the pinned harness, plus /media/ and /cert/ from the verified vector cache."""

    manifest = {}
    recorded = {}

    def log_message(self, *_a):  # silence: one line per media range request is unreadable
        pass

    def do_GET(self):
        return self._route()

    def do_HEAD(self):
        # §6.5 preflights the vectors with HEAD. Delegating to do_GET would ship a body on a HEAD,
        # which most clients tolerate and none should have to.
        return self._route()

    def _route(self):
        # `BYPASS_CACHE` in harness/xhr.js appends `?<epoch-ms>` to every URL, so never match on
        # self.path raw.
        path = self.path.split("?", 1)[0]
        if path == ch.ECHO_ROUTE:
            return self._serve_echo()
        if path == "/harness/util.js":
            return self._serve_rewritten_util()
        # Vectors live under `/__vectors/`, NOT `/media/`: the harness has its own `media/` directory
        # full of suite definitions, and shadowing it sends those script tags to Google's bucket.
        if path.startswith(ch.VECTOR_PREFIX):
            rest = path[len(ch.VECTOR_PREFIX):]
            kind, _, rel = rest.partition("/")
            if kind in ("media", "cert") and rel:
                return self._serve_vector(kind, rel)
            self.send_error(404, "unknown vector route")
            return
        return super().do_GET() if self.command == "GET" else super().do_HEAD()

    def do_POST(self):
        return self._route()

    def _serve_echo(self):
        """Reflect the request body. See ch.ECHO_ROUTE."""
        if self.command != "POST":
            self.send_error(405, "echo is POST-only")
            return
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.send_error(400, "bad Content-Length")
            return
        # Bound it. The route exists for an 8-byte test; an unbounded read here is a way to hang the
        # only thread that can tell you the suite hung.
        if n < 0 or n > ECHO_MAX_BYTES:
            self.send_error(413, f"echo body must be 0..{ECHO_MAX_BYTES} bytes")
            return
        body = self.rfile.read(n) if n else b""
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self._write(body)

    def _write(self, blob):
        """The harness aborts range requests by design (XHRAbort issues 100 and cancels 99). Each
        abort lands here as a BrokenPipeError, and socketserver answers it with a full traceback on
        stderr — a hundred of them, in the middle of the one log a human reads to find out why the
        suite failed. The client hanging up is not our error."""
        try:
            self.wfile.write(blob)
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True

    def _serve_rewritten_util(self):
        src = (Path(self.directory) / "harness" / "util.js").read_text()
        try:
            body = ch.rewrite_util_js(src).encode()
        except ch.HarnessError as e:
            self.send_error(500, str(e))
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/javascript")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self._write(body)

    def _serve_vector(self, kind, rel):
        if ".." in rel:
            self.send_error(400, "no")
            return
        try:
            blob = fetch_vector(kind, rel, self.manifest, self.recorded)
        except Env as e:
            # 502: the *upstream* is at fault. The harness will fail the test; cert.py's preflight
            # and the recorded reason are what tell you which of the two happened.
            self.send_error(502, str(e))
            return
        # The harness byte-ranges everything, so range support is not optional.
        rng = self.headers.get("Range")
        total = len(blob)
        if rng and rng.startswith("bytes="):
            first, _, last = rng[6:].partition("-")
            start = int(first) if first else 0
            end = int(last) if last else total - 1
            end = min(end, total - 1)
            if start > end:
                self.send_error(416, "bad range")
                return
            chunk = blob[start:end + 1]
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{total}")
        else:
            chunk = blob
            self.send_response(200)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(chunk)))
        self.end_headers()
        if self.command != "HEAD":
            self._write(chunk)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        # Same reason as Handler._write: a client that hung up is not a server error. Anything else
        # still gets its traceback, because a server bug during a conformance run is exactly the sort
        # of thing that otherwise gets blamed on the decoder.
        exc = sys.exc_info()[1]
        if isinstance(exc, (BrokenPipeError, ConnectionResetError)):
            return
        super().handle_error(request, client_address)


@contextlib.contextmanager
def serve(root, port):
    """Bind 127.0.0.1 only: the harness must never be reachable from the LAN, and the origin must be
    `localhost` for both the secure-context and the phone-home reasons above."""
    Handler.manifest = load_manifest()
    Handler.recorded = {}
    handler = functools.partial(Handler, directory=str(root))
    httpd = Server(("127.0.0.1", port), handler)
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    try:
        yield httpd
    finally:
        httpd.shutdown()
        httpd.server_close()


def port_is_free(port):
    """Answer the question `Server` will actually ask, not a stricter one.

    `Server.allow_reuse_address` sets SO_REUSEADDR, so it binds over a TIME_WAIT socket left by the
    previous run. This guard did not, so it reported "port 8000 is busy" for a port the server was
    about to bind without complaint — two `just cert` runs in a row, second one exit 3. A precondition
    check that is stricter than the precondition is a false alarm generator.
    """
    with socket.socket() as s:
        if Server.allow_reuse_address:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False


# ---- driving the engine --------------------------------------------------------------------------

def run_suite(cdp, base_url, suite, args):
    """Navigate, then poll `globalRunner.testList` until nothing is UNKNOWN.

    Two independent deadlines, because a conformance harness that wedges on one test will otherwise
    hold a runner forever:

      * a per-suite wall clock, and
      * a **no-progress watchdog** — if the number of finished tests does not increase within
        `--no-progress` seconds, abort and name the test that was in flight.

    `stoponfailure` stays off. We want the whole result vector, not the first failure.
    """
    url = f"{base_url}/main.html?test_type={suite}&command=run&timeout={args.test_timeout_ms}"
    _say(f"cert: navigating to {url}")
    cdp.call("Page.navigate", {"url": url})

    ok = cdp.wait_for("!!(window.globalRunner && window.globalRunner.testList)", timeout=60)
    if not ok:
        raise ch.HarnessError("the harness never installed window.globalRunner (page failed to load)")

    verdict, reason = ch.preflight_verdict(cdp.evaluate(ch.PREFLIGHT_EXPR))
    if not verdict:
        raise Env(reason)
    _say(f"cert: preflight ok — {reason}")

    deadline = time.monotonic() + args.suite_timeout
    last_done, last_progress_at = -1, time.monotonic()
    tests = []
    while time.monotonic() < deadline:
        tests = ch.parse_scrape(cdp.evaluate(ch.SCRAPE_EXPR))
        done, total = ch.progress(tests)
        if done > last_done:
            last_done, last_progress_at = done, time.monotonic()
            _say(f"cert: {done}/{total}")
        if done == total and total > 0:
            return tests
        if time.monotonic() - last_progress_at > args.no_progress:
            stuck = ch.in_flight(tests)
            name = stuck["name"] if stuck else "<none>"
            RESULT_DIR.mkdir(parents=True, exist_ok=True)
            cdp.screenshot(str(RESULT_DIR / f"{suite}-wedged.png"))
            raise TimeoutError(
                f"no progress for {args.no_progress}s at {done}/{total}. In flight: {name!r}. "
                "Screenshot saved. The suite is wedged; a partial result vector has no opinion "
                "about the tests it never reached."
            )
        time.sleep(2)
    raise TimeoutError(f"suite {suite} exceeded its {args.suite_timeout}s wall clock")


def rerun_failures(cdp, base_url, suite, tests, args):
    """Re-run only the failed indices for flake isolation. A test that passes on re-run is `flaky`.

    §0's rule reaches here: retry transport, never adjudication. A green re-run does not delete the
    red one, it renames it — so the second result is recorded as `flaky` and reported, never as a pass.
    """
    failed = [t for t in tests if t["outcome"] in (ch.FAILED, ch.TIMEOUT)]
    if not failed:
        return {}
    idx = ",".join(str(t["index"] + 1) for t in failed)  # the harness's command= indices are 1-based
    _say(f"cert: re-running {len(failed)} failed test(s) for flake isolation")
    url = f"{base_url}/main.html?test_type={suite}&command=run:{idx}&timeout={args.test_timeout_ms}"
    cdp.call("Page.navigate", {"url": url})
    cdp.wait_for("!!(window.globalRunner && window.globalRunner.testList)", timeout=60)

    deadline = time.monotonic() + args.suite_timeout
    wanted = {t["name"] for t in failed}
    while time.monotonic() < deadline:
        again = ch.parse_scrape(cdp.evaluate(ch.SCRAPE_EXPR))
        subset = [t for t in again if t["name"] in wanted]
        if subset and all(t["outcome"] in ch.TERMINAL for t in subset):
            by_name = {t["name"]: t["outcome"] for t in subset}
            return {t["name"]: ch.flake_verdict(t["outcome"], by_name.get(t["name"], ch.UNKNOWN))
                    for t in failed}
        time.sleep(2)
    _say("cert: the re-run did not finish; recording the original outcomes unchanged")
    return {}


# ---- main ----------------------------------------------------------------------------------------

def load_baseline(suite):
    path = EXPECT_DIR / f"{suite}.json"
    if not path.exists():
        raise Env(
            f"no baseline at {_rel(path)}.\n"
            "A conformance run with nothing to compare against cannot pass or fail — some tests are\n"
            "legitimately OPTIONAL_FAILED and some fail on known gaps of ours. Record one:\n"
            f"    scripts/cert.py --suite {suite} --update-baseline\n"
            "then READ the diff before committing it. A baseline is a claim about what should pass."
        )
    return json.loads(path.read_text())


def report(suite, tests, verdict, flakes, meta):
    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    counts = ch.summarize(tests)
    _say(f"\ncert: {suite}: " + ", ".join(f"{k}={v}" for k, v in counts.items() if v))

    # Every non-passing test, with the harness's own error text. Printed on EVERY run, including
    # `--update-baseline`, because a baseline is a claim about what should pass and nobody can review
    # that claim without seeing why the failures failed. Rubber-stamping the first run bakes today's
    # bugs in as tomorrow's expectations.
    failures = [t for t in tests if t["outcome"] != ch.PASSED]
    if failures:
        _say(f"cert: {len(failures)} test(s) did not pass — READ THESE before trusting a baseline:")
        for t in failures:
            _say(f"  {ch.OUTCOME_NAMES.get(t['outcome'], '?'):16} {t['name']}"
                 f"{' (optional)' if not t['mandatory'] else ''}")
            if t.get("error"):
                _say(f"      {t['error'][:400]}")

    for t in verdict.regressions:
        _say(f"  REGRESSION  {t['name']}: expected {t['expected']}, got {t['actual']}"
             + (f" — {t['error']}" if t.get("error") else ""))
    for t in verdict.unknown:
        _say(f"  NEVER RAN   {t['name']}")
    for t in verdict.newly_passing:
        _say(f"  now passing {t['name']} (baseline says {t['expected']}) — baseline update available")
    for t in verdict.new_tests:
        _say(f"  new test    {t['name']} (not in the baseline)")
    for name in verdict.missing_tests:
        _say(f"  vanished    {name} (in the baseline, not in this run)")
    for name, kind in sorted(flakes.items()):
        if kind == "flaky":
            _say(f"  FLAKY       {name} (failed, then passed on re-run — recorded as flaky, not pass)")

    (RESULT_DIR / f"{suite}.json").write_text(json.dumps(
        {"suite": suite, "meta": meta, "counts": counts, "tests": tests,
         "flakes": flakes,
         "regressions": [t["name"] for t in verdict.regressions],
         "unknown": [t["name"] for t in verdict.unknown],
         "newly_passing": [t["name"] for t in verdict.newly_passing]},
        indent=2))
    (RESULT_DIR / f"{suite}.xml").write_text(ch.junit_xml(suite, tests, verdict, meta))
    _say(f"cert: wrote {_rel(RESULT_DIR)}/{suite}.{{json,xml}}")


def main():
    ap = _Parser(description=__doc__.splitlines()[0])
    ap.add_argument("--suite", default="conformance-test")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--cdp-port", type=int, default=9222)
    ap.add_argument("--deck", action="store_true",
                    help="run against the Deck over SSH. The app must already be running there with "
                         "--remote-debugging-port (just run / just remote-run).")
    ap.add_argument("--host", default=None)
    ap.add_argument("--suite-timeout", type=float, default=3600.0)
    ap.add_argument("--no-progress", type=float, default=180.0,
                    help="abort if no test finishes within this many seconds")
    ap.add_argument("--test-timeout-ms", type=int, default=40000)
    ap.add_argument("--update-baseline", action="store_true",
                    help="record this run as the baseline. Never do this in CI.")
    ap.add_argument("--freeze-vectors", action="store_true",
                    help="write the SHA-256 of every vector this run fetched")
    ap.add_argument("--sync-only", action="store_true", help="fetch the harness and exit")
    ap.add_argument("--no-rerun", action="store_true", help="skip flake isolation")
    args = ap.parse_args()

    if args.update_baseline and os.environ.get("CI"):
        _say("--update-baseline is refused under CI: a baseline that rewrites itself agrees with "
             "whatever just happened, which is not agreement.")
        return EX_USAGE

    pins = read_pins()
    root = sync_harness(pins)
    if args.sync_only:
        return EX_OK

    if not args.update_baseline:
        baseline = load_baseline(args.suite)
    else:
        baseline = {}

    if not port_is_free(args.port):
        raise Env(f"port {args.port} is busy; the harness cannot be served there")

    meta = {"suite": args.suite, "target": "deck" if args.deck else "workstation"}
    # Every pin goes into the result JSON. A baseline whose harness you cannot name is not a baseline
    # — and "the harness" here is three repositories, not one.
    for pin in pins:
        meta[f"{pin.name}_commit"] = pin.sha

    with serve(root, args.port) as _httpd:
        base_url = f"http://localhost:{args.port}"
        stack = contextlib.ExitStack()
        with stack:
            if args.deck:
                host = args.host or sshlib.deck_host()
                if not host:
                    raise Env("no DECK_HOST")
                port = sshlib.deck_port()
                if not sshlib.reachable(host, port):
                    _say(f"{host}:{port} unreachable")
                    return EX_TRANSPORT
                # -R makes the workstation's server appear at localhost:PORT *on the Deck*, which is
                # the only way the engine there gets a secure context for EME.
                stack.enter_context(reverse_tunnel(host, port, args.port))
                stack.enter_context(sshlib.Tunnel(host, port, args.cdp_port))
            # On the Deck the app under test is driven by deckback-launcher, whose Navigator owns the
            # existing page and pulls it back to youtube.com/tv the moment we navigate away -- the
            # suite then reports "the harness never installed window.globalRunner". Take our own page.
            cdp = stack.enter_context(cdplib.CDP(args.cdp_port, own_target=args.deck))
            cdp.call("Page.enable")
            cdp.call("Runtime.enable")

            tests = run_suite(cdp, base_url, args.suite, args)
            flakes = {} if args.no_rerun else rerun_failures(cdp, base_url, args.suite, tests, args)

        if args.freeze_vectors and Handler.recorded:
            merged = {**load_manifest(), **Handler.recorded}
            save_manifest(merged)
            _say(f"cert: froze {len(Handler.recorded)} new vector hash(es) into "
                 f"{_rel(VECTOR_MANIFEST)}")
        elif Handler.recorded:
            _say(f"cert: {len(Handler.recorded)} vector(s) served with no recorded hash. "
                 "Run with --freeze-vectors once, then review the manifest.")

    meta["vectors_recorded"] = len(Handler.recorded)

    verdict = ch.adjudicate(tests, baseline)
    report(args.suite, tests, verdict, flakes, meta)

    if args.update_baseline:
        EXPECT_DIR.mkdir(parents=True, exist_ok=True)
        path = EXPECT_DIR / f"{args.suite}.json"
        path.write_text(json.dumps(ch.as_baseline(tests), indent=2, sort_keys=True) + "\n")
        _say(f"\ncert: wrote {_rel(path)}. READ IT before committing — it is a claim "
             "about what should pass, not a record of what did. The failures printed above are "
             "exactly what you would be blessing.")
        return EX_OK

    if args.suite in TREND_SUITES:
        # §6.3: playbackperf depends on TDP, refresh rate, and thermals. Recorded, never a gate.
        _say(f"cert: {args.suite} is a TREND, not a gate — results recorded, exit 0 regardless.")
        return EX_OK
    return EX_ASSERT if verdict.failed else EX_OK


@contextlib.contextmanager
def reverse_tunnel(host, ssh_port, http_port):
    argv = ["ssh", "-p", str(ssh_port), "-N", "-o", "BatchMode=yes",
            "-o", "ExitOnForwardFailure=yes",
            "-R", f"{http_port}:localhost:{http_port}", host]
    proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    time.sleep(1.5)
    if proc.poll() is not None:
        err = proc.stderr.read().decode(errors="replace").strip()
        raise sshlib.DeckUnreachable(f"reverse tunnel failed: {err}")
    try:
        yield proc
    finally:
        proc.terminate()
        with contextlib.suppress(subprocess.TimeoutExpired):
            proc.wait(timeout=5)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Env as e:
        _say(f"environment: {e}")
        sys.exit(EX_ENV)
    except ch.HarnessError as e:
        _say(f"harness: {e}")
        sys.exit(EX_ENV)
    except TimeoutError as e:
        # A wedged suite has no opinion about the tests it never reached, and silence is not assent.
        _say(f"assertion: {e}")
        sys.exit(EX_ASSERT)
    except sshlib.DeckUnreachable as e:
        _say(f"transport: {e}")
        sys.exit(EX_TRANSPORT)
    except cdplib.RETRYABLE as e:
        _say(f"transport: {type(e).__name__}: {e}")
        sys.exit(EX_TRANSPORT)
