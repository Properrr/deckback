"""Pure logic for the L3 conformance gate (no I/O; `scripts/cert.py` does the talking).

- Results come from `globalRunner.testList`, never `getTestResults()`, which silently drops
  UNKNOWN (never ran) and TIMEOUT tests.
- A run with no recorded baseline does not pass (exit 3).
- Only regressions (PASSED in the baseline, not PASSED now) fail; newly-passing tests are reported,
  never written back to the baseline automatically.
"""

from __future__ import annotations

import json
import re
import xml.etree.ElementTree as ET

# harness/test.js:TestOutcome.
UNKNOWN, PASSED, FAILED, OPTIONAL_FAILED, TIMEOUT = 0, 1, 2, 3, 4
OUTCOME_NAMES = {
    UNKNOWN: "UNKNOWN",
    PASSED: "PASSED",
    FAILED: "FAILED",
    OPTIONAL_FAILED: "OPTIONAL_FAILED",
    TIMEOUT: "TIMEOUT",
}

# A run is finished when nothing is UNKNOWN (never started); TIMEOUT is a result.
TERMINAL = (PASSED, FAILED, OPTIONAL_FAILED, TIMEOUT)

# `lastError` is an Error object, so only its message survives the CDP round trip.
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

# EME tests fail on an insecure origin, and the harness POSTs results outward when the page URL
# contains appspot.com/googleapis.com (harness/test.js); both are checked before any test runs.
PREFLIGHT_EXPR = (
    "JSON.stringify({secure: !!window.isSecureContext, host: location.host, "
    "href: location.href})"
)

PHONE_HOME_MARKERS = ("appspot.com", "googleapis.com")


class HarnessError(RuntimeError):
    """The harness or its hosting is wrong. ENVIRONMENT — never a decoder regression."""


def preflight_verdict(raw):
    """(ok, reason): the origin must be a secure context and its URL free of phone-home markers."""
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
    """(done, total), counting terminal outcomes only."""
    return (sum(1 for t in tests if t["outcome"] in TERMINAL), len(tests))


def in_flight(tests):
    """The first test that has not reached a terminal outcome, or None."""
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
    """{name: OUTCOME_NAME}, keyed by name because indices shift when the harness adds a test."""
    return {t["name"]: OUTCOME_NAMES.get(t["outcome"], "UNKNOWN") for t in tests}


class Verdict:
    """What changed against the committed baseline. Only `regressions` fails the build."""

    def __init__(self):
        self.regressions = []      # was PASSED, now is not
        self.newly_passing = []    # baseline says not-PASSED, run says PASSED
        self.unknown = []          # never ran: a wedged or truncated suite
        self.new_tests = []        # in the run, absent from the baseline
        self.missing_tests = []    # in the baseline, absent from the run

    @property
    def failed(self):
        # A wedged/truncated run (UNKNOWN) fails too.
        return bool(self.regressions or self.unknown)

    def __repr__(self):
        return (f"<Verdict regressions={len(self.regressions)} unknown={len(self.unknown)} "
                f"newly_passing={len(self.newly_passing)} new={len(self.new_tests)} "
                f"missing={len(self.missing_tests)}>")


def adjudicate(tests, baseline):
    """Compare a run against a recorded `{test_name: OUTCOME_NAME}` baseline.

    Never pass `{}`: every test would be a `new_test` and the run would pass.
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
    """Returns pass, fail, or flaky (failed, then passed on re-run — never pass)."""
    if first_outcome == PASSED:
        return "pass"
    if rerun_outcome == PASSED:
        return "flaky"
    return "fail"


# ---- serving the harness -------------------------------------------------------------------------

# harness/util.js's MEDIA_PATH/CERT_PATH point at Google's bucket; we rewrite them to same-origin
# routes served from a verified local cache.
_MEDIA_RE = re.compile(r"^const MEDIA_PATH = '[^']*';", re.M)
_CERT_RE = re.compile(r"^const CERT_PATH = '[^']*';", re.M)

REMOTE_MEDIA_BASE = "https://storage.googleapis.com/ytlr-cert.appspot.com/test-materials/media/"
REMOTE_CERT_BASE = "https://storage.googleapis.com/ytlr-cert.appspot.com/test-materials/cert/"

# Vector routes must not shadow the harness's own tree (its suites live in `media/`); an L0 test
# asserts this prefix matches nothing in the vendored tree.
VECTOR_PREFIX = "/__vectors/"
MEDIA_ROUTE = VECTOR_PREFIX + "media/"
CERT_ROUTE = VECTOR_PREFIX + "cert/"

# Server route the harness expects but does not ship: `XHRUint8Array` POSTs eight bytes here and
# expects them echoed with a 2xx; anything else surfaces as that test TIMING OUT.
ECHO_ROUTE = "/echo"


def rewrite_util_js(source, media_path=MEDIA_ROUTE, cert_path=CERT_ROUTE):
    """Point MEDIA_PATH/CERT_PATH at our own origin.

    Raises HarnessError if either constant is missing, so a harness change cannot silently fall back
    to loading unhashed vectors from Google's bucket.
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
            # Known-failing per the baseline: recorded, not a failure.
            ET.SubElement(case, "system-out").text = f"{outcome} (expected, per baseline): {t.get('error','')}"
    return ET.tostring(ts, encoding="unicode")
