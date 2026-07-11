"""Pure logic for the L3 conformance gate (TEST-PLAN §6). No sockets, no browser, no filesystem.

Everything here is a function of its arguments so that `tests/harness/test_cert.py` can check it on
a laptop with no Chromium checkout. The parts that talk to the world live in `scripts/cert.py`.

Three decisions are encoded here, and each one is the difference between a gate and a decoration:

1. **We scrape `globalRunner.testList`, never `getTestResults()`.** The harness's own convenience
   function buckets tests into `pass` and `fail` and *silently drops* `UNKNOWN` (never ran) and
   `TIMEOUT`. A suite that wedges on test 3 of 400 therefore reports zero failures. Read
   `harness/test.js:getTestResults` if you doubt it.

2. **A run with no baseline does not pass.** It exits 3 and tells you how to record one. Some tests
   are legitimately `OPTIONAL_FAILED` and some fail on known gaps of ours, so a suite that is red on
   day one is ignored by day three (TEST-PLAN §6.2).

3. **Only regressions fail the build.** A test that was `PASSED` and now is not. A newly-passing test
   prints "baseline update available" and never auto-updates in CI: a baseline that rewrites itself
   agrees with whatever happened last, which is not agreement.
"""

from __future__ import annotations

import json
import re
import xml.etree.ElementTree as ET

# harness/test.js:TestOutcome. Ours to read, never ours to choose.
UNKNOWN, PASSED, FAILED, OPTIONAL_FAILED, TIMEOUT = 0, 1, 2, 3, 4
OUTCOME_NAMES = {
    UNKNOWN: "UNKNOWN",
    PASSED: "PASSED",
    FAILED: "FAILED",
    OPTIONAL_FAILED: "OPTIONAL_FAILED",
    TIMEOUT: "TIMEOUT",
}

# A run is finished when nothing is UNKNOWN. TIMEOUT is a *result*: the harness reached the test and
# the test ran out of time. UNKNOWN means it never started.
TERMINAL = (PASSED, FAILED, OPTIONAL_FAILED, TIMEOUT)

# Walk the test list directly. `lastError` is an Error object, so only its message survives the CDP
# round trip; `mandatory` distinguishes a real FAILED from an OPTIONAL_FAILED the suite tolerates.
SCRAPE_EXPR = """
(function(){
  if (!window.globalRunner || !window.globalRunner.testList) return JSON.stringify(null);
  var out = [];
  for (var i = 0; i < window.globalRunner.testList.length; i++) {
    var p = window.globalRunner.testList[i].prototype;
    out.push({
      index: i,
      name: String(p.name),
      category: String(p.category || ''),
      mandatory: !!p.mandatory,
      outcome: p.outcome | 0,
      error: (p.lastError && p.lastError.message) ? String(p.lastError.message) : ''
    });
  }
  return JSON.stringify(out);
})()
"""

# §6.5: every EME test fails instantly on an insecure origin, and the harness POSTs results outward
# when the page host contains appspot.com/googleapis.com (harness/test.js:387). Both are properties
# of the *origin*, so both are checked before a single test runs.
PREFLIGHT_EXPR = (
    "JSON.stringify({secure: !!window.isSecureContext, host: location.host, "
    "href: location.href})"
)

PHONE_HOME_MARKERS = ("appspot.com", "googleapis.com")


class HarnessError(RuntimeError):
    """The harness or its hosting is wrong. ENVIRONMENT — never a decoder regression."""


def preflight_verdict(raw):
    """(ok, reason) for the origin the harness is about to run on.

    An insecure origin makes every EME test fail at `requestMediaKeySystemAccess`, for a reason that
    has nothing to do with our engine. And a page whose URL contains `appspot.com`/`googleapis.com`
    POSTs its results to `qual-e.appspot.com` — we do not send test results to third parties.
    """
    d = json.loads(raw)
    if not d.get("secure"):
        return (False, f"insecure context at {d.get('href')!r}. EME needs a potentially-trustworthy "
                       "origin; http://localhost qualifies and a LAN IP does not. Every EME test "
                       "would fail for reasons unrelated to the engine.")
    href = str(d.get("href", ""))
    for marker in PHONE_HOME_MARKERS:
        if marker in href:
            return (False, f"the page URL contains {marker!r}, which makes the harness POST results "
                           f"to qual-e.appspot.com (harness/test.js). Serve from localhost.")
    return (True, f"secure context on {d.get('host')!r}, no phone-home")


def parse_scrape(raw):
    """CDP's reply -> list of test dicts. `null` means the harness never installed globalRunner."""
    data = json.loads(raw) if raw else None
    if data is None:
        raise HarnessError(
            "window.globalRunner is missing. The harness page did not load or threw before "
            "installing its runner (check the served files and the browser console)."
        )
    return data


def progress(tests):
    """(done, total). Used by the no-progress watchdog, so it counts *terminal* outcomes only."""
    return (sum(1 for t in tests if t["outcome"] in TERMINAL), len(tests))


def in_flight(tests):
    """The first test that has not reached a terminal outcome. Names the wedge for the log."""
    for t in tests:
        if t["outcome"] not in TERMINAL:
            return t
    return None


def summarize(tests):
    counts = {name: 0 for name in OUTCOME_NAMES.values()}
    for t in tests:
        counts[OUTCOME_NAMES.get(t["outcome"], "UNKNOWN")] += 1
    return counts


def as_baseline(tests):
    """{name: OUTCOME_NAME}. Names, not indices: indices shift when the harness adds a test."""
    return {t["name"]: OUTCOME_NAMES.get(t["outcome"], "UNKNOWN") for t in tests}


class Verdict:
    """What changed against the committed baseline. Only `regressions` fails the build."""

    def __init__(self):
        self.regressions = []      # was PASSED, now is not. The gate.
        self.newly_passing = []    # baseline says not-PASSED, run says PASSED. Report, never update.
        self.unknown = []          # never ran. A wedged or truncated suite.
        self.new_tests = []        # in the run, absent from the baseline.
        self.missing_tests = []    # in the baseline, absent from the run.

    @property
    def failed(self):
        # UNKNOWN is a failure of the *run*, not of the engine, but it must never pass: a suite that
        # stopped after 3 of 400 tests has no opinion about the other 397, and silence is not assent.
        return bool(self.regressions or self.unknown)

    def __repr__(self):
        return (f"<Verdict regressions={len(self.regressions)} unknown={len(self.unknown)} "
                f"newly_passing={len(self.newly_passing)} new={len(self.new_tests)} "
                f"missing={len(self.missing_tests)}>")


def adjudicate(tests, baseline):
    """Compare a run against `{test_name: OUTCOME_NAME}`.

    `baseline` must be a real recorded baseline. Passing `{}` here would make every test a
    `new_test` and the run would pass with no opinion at all — so `cert.py` refuses to run without
    one rather than letting this function paper over its absence.
    """
    v = Verdict()
    seen = set()
    for t in tests:
        name, actual = t["name"], OUTCOME_NAMES.get(t["outcome"], "UNKNOWN")
        seen.add(name)
        if actual == "UNKNOWN":
            v.unknown.append(t)
            continue
        if name not in baseline:
            v.new_tests.append(t)
            continue
        expected = baseline[name]
        if expected == "PASSED" and actual != "PASSED":
            v.regressions.append({**t, "expected": expected, "actual": actual})
        elif expected != "PASSED" and actual == "PASSED":
            v.newly_passing.append({**t, "expected": expected})
    v.missing_tests = sorted(set(baseline) - seen)
    return v


def flake_verdict(first_outcome, rerun_outcome):
    """A test that failed and then passed on re-run is `flaky`, never `pass` (TEST-PLAN §0, §6.2).

    Retry transport, never adjudication. A green re-run does not delete the red one; it renames it.
    """
    if first_outcome == PASSED:
        return "pass"
    if rerun_outcome == PASSED:
        return "flaky"
    return "fail"


# ---- serving the harness -------------------------------------------------------------------------

# harness/util.js pins the vectors to a third-party bucket whose sibling endpoints Google has already
# sunset. We rewrite both constants to same-origin paths and serve from a verified local cache.
_MEDIA_RE = re.compile(r"^const MEDIA_PATH = '[^']*';", re.M)
_CERT_RE = re.compile(r"^const CERT_PATH = '[^']*';", re.M)

REMOTE_MEDIA_BASE = "https://storage.googleapis.com/ytlr-cert.appspot.com/test-materials/media/"
REMOTE_CERT_BASE = "https://storage.googleapis.com/ytlr-cert.appspot.com/test-materials/cert/"

# The vector routes must not shadow the harness's own tree. `/media/` did: js_mse_eme keeps its suite
# definitions in `media/` (`media/conformanceTest.js` and friends, loaded by `main.html` as ordinary
# script tags). Serving vectors at `/media/` sent those script requests to Google's bucket, where
# they 404, so the harness never defined its tests and never created `window.globalRunner`. The
# failure surfaced as "the harness never installed window.globalRunner" — a message about the engine.
#
# `__vectors` cannot collide: no path in the harness begins with an underscore, and an L0 test
# asserts the prefix matches nothing in the vendored tree.
VECTOR_PREFIX = "/__vectors/"
MEDIA_ROUTE = VECTOR_PREFIX + "media/"
CERT_ROUTE = VECTOR_PREFIX + "cert/"

# The one route the harness hardcodes and does not ship. `XHRUint8Array` POSTs eight bytes here and
# asserts they come back unchanged; Google's runner answered it server-side, and the source contains
# no hint that a route is expected. Serve anything else — 404, 501, a stray SimpleHTTPRequestHandler
# — and the XHR's `load` handler trips `logger.assert(status 2xx)`, `runner.succeed()` is never
# called, and ten seconds later the harness reports `XHRUint8Array TIMED OUT`. A missing server route
# that presents as an engine timeout: assume nothing about a red conformance test until you have read
# what it does.
ECHO_ROUTE = "/echo"


def rewrite_util_js(source, media_path=MEDIA_ROUTE, cert_path=CERT_ROUTE):
    """Point MEDIA_PATH/CERT_PATH at our own origin. Raises if either constant is not there.

    The raise is the point. If a harness bump renames these constants, a silent no-op rewrite would
    send every media request straight back to `storage.googleapis.com` — the run would still pass,
    the hash pinning would be bypassed, and nobody would know until the bucket disappeared.
    """
    out, n_media = _MEDIA_RE.subn(f"const MEDIA_PATH = '{media_path}';", source, count=1)
    out, n_cert = _CERT_RE.subn(f"const CERT_PATH = '{cert_path}';", out, count=1)
    if n_media != 1 or n_cert != 1:
        raise HarnessError(
            f"could not rewrite harness/util.js (MEDIA_PATH x{n_media}, CERT_PATH x{n_cert}). "
            "The harness changed shape; the vectors would silently load from Google's bucket, "
            "unhashed, and the run would still go green."
        )
    return out


# ---- artifacts -----------------------------------------------------------------------------------

def junit_xml(suite, tests, verdict, properties=None):
    """JUnit XML. A regression is a `failure`; an UNKNOWN is an `error` (the run, not the engine)."""
    ts = ET.Element("testsuite", {
        "name": suite,
        "tests": str(len(tests)),
        "failures": str(len(verdict.regressions)),
        "errors": str(len(verdict.unknown)),
        "skipped": "0",
    })
    if properties:
        props = ET.SubElement(ts, "properties")
        for k, val in sorted(properties.items()):
            ET.SubElement(props, "property", {"name": str(k), "value": str(val)})

    regressed = {t["name"] for t in verdict.regressions}
    unknown = {t["name"] for t in verdict.unknown}
    for t in tests:
        case = ET.SubElement(ts, "testcase", {
            "classname": f"{suite}.{t['category'] or 'uncategorized'}",
            "name": t["name"],
        })
        outcome = OUTCOME_NAMES.get(t["outcome"], "UNKNOWN")
        if t["name"] in regressed:
            ET.SubElement(case, "failure", {"message": f"regression: expected PASSED, got {outcome}"}
                          ).text = t.get("error", "")
        elif t["name"] in unknown:
            ET.SubElement(case, "error", {"message": "never ran (suite did not reach this test)"})
        elif outcome != "PASSED":
            # Known-failing per the baseline. Recorded, visible, and not a build failure.
            ET.SubElement(case, "system-out").text = f"{outcome} (expected, per baseline): {t.get('error','')}"
    return ET.tostring(ts, encoding="unicode")
