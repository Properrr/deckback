#!/usr/bin/env python3
"""L0 coverage of the L3 conformance gate — no browser, no Chromium checkout, no network.

`just cert` needs a `content_shell` build to run, so it will not run in CI for a while yet. What CAN
be checked here is every decision it makes once the numbers are in, and those decisions are the whole
gate. In particular:

  * A wedged suite must never pass. The harness's own `getTestResults()` buckets tests into `pass`
    and `fail` and silently drops `UNKNOWN` and `TIMEOUT`, so a suite that dies on test 3 of 400
    reports zero failures. We scrape `globalRunner.testList` instead, and `UNKNOWN` fails the run.
  * A run with no baseline must not pass either. It has nothing to have an opinion about.
  * A vector whose hash moved is Google changing a bucket (exit 3), not our decoder breaking (2).
"""

import functools
import json
import os
import sys
import socket
import subprocess
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

_HERE = Path(os.path.dirname(os.path.abspath(__file__)))
_REPO = _HERE.parent.parent
sys.path.insert(0, str(_REPO / "scripts"))
sys.path.insert(0, str(_REPO / "scripts" / "lib"))
sys.path.insert(0, str(_REPO / "tests" / "deck"))

import cert  # noqa: E402
import cert_harness as ch  # noqa: E402


def T(name, outcome, mandatory=True, category="cat", index=0, error=""):
    return {"index": index, "name": name, "category": category, "mandatory": mandatory,
            "outcome": outcome, "error": error}


class Scrape(unittest.TestCase):
    def test_missing_globalrunner_raises(self):
        # A blank page and a page whose harness threw look identical from here. Both are ENV.
        with self.assertRaises(ch.HarnessError):
            ch.parse_scrape("null")

    def test_progress_counts_terminal_outcomes_only(self):
        tests = [T("a", ch.PASSED), T("b", ch.TIMEOUT), T("c", ch.UNKNOWN)]
        self.assertEqual(ch.progress(tests), (2, 3))

    def test_timeout_is_a_result_and_unknown_is_not(self):
        # The harness reached the TIMEOUT test and it ran out of time; it never reached the UNKNOWN
        # one. Treating them alike makes the no-progress watchdog either blind or hysterical.
        self.assertIn(ch.TIMEOUT, ch.TERMINAL)
        self.assertNotIn(ch.UNKNOWN, ch.TERMINAL)

    def test_in_flight_names_the_wedge(self):
        tests = [T("a", ch.PASSED), T("stuck", ch.UNKNOWN), T("c", ch.UNKNOWN)]
        self.assertEqual(ch.in_flight(tests)["name"], "stuck")
        self.assertIsNone(ch.in_flight([T("a", ch.PASSED)]))


class Preflight(unittest.TestCase):
    def test_insecure_origin_is_refused(self):
        # Every EME test would fail at requestMediaKeySystemAccess, for a reason that has nothing to
        # do with our engine. Catch it before a single test runs.
        ok, why = ch.preflight_verdict(json.dumps(
            {"secure": False, "host": "192.168.1.7:8000", "href": "http://192.168.1.7:8000/main.html"}))
        self.assertFalse(ok)
        self.assertIn("insecure", why)

    def test_a_phone_home_origin_is_refused(self):
        # harness/test.js POSTs results to qual-e.appspot.com when the page URL says appspot/googleapis.
        for host in ("ytlr-cert.appspot.com", "storage.googleapis.com"):
            ok, why = ch.preflight_verdict(json.dumps(
                {"secure": True, "host": host, "href": f"https://{host}/main.html"}))
            self.assertFalse(ok, host)
            self.assertIn("qual-e", why)

    def test_localhost_passes(self):
        ok, _ = ch.preflight_verdict(json.dumps(
            {"secure": True, "host": "localhost:8000", "href": "http://localhost:8000/main.html"}))
        self.assertTrue(ok)


class Adjudication(unittest.TestCase):
    def test_a_regression_fails(self):
        v = ch.adjudicate([T("a", ch.FAILED)], {"a": "PASSED"})
        self.assertTrue(v.failed)
        self.assertEqual(v.regressions[0]["actual"], "FAILED")

    def test_a_known_failure_does_not_fail_the_build(self):
        # Some tests are legitimately OPTIONAL_FAILED and some fail on known gaps of ours. A suite
        # red on day one is ignored by day three, which is how the gate dies.
        v = ch.adjudicate([T("a", ch.OPTIONAL_FAILED)], {"a": "OPTIONAL_FAILED"})
        self.assertFalse(v.failed)

    def test_a_never_run_test_fails_the_run(self):
        # THE bug this file exists for. A suite that wedged after 1 of 3 tests has no opinion about
        # the other two, and silence is not assent. `getTestResults()` would report zero failures.
        v = ch.adjudicate([T("a", ch.PASSED), T("b", ch.UNKNOWN)], {"a": "PASSED", "b": "PASSED"})
        self.assertTrue(v.failed)
        self.assertEqual([t["name"] for t in v.unknown], ["b"])
        self.assertEqual(v.regressions, [], "an UNKNOWN is not a regression; it is a broken run")

    def test_newly_passing_is_reported_and_never_auto_applied(self):
        v = ch.adjudicate([T("a", ch.PASSED)], {"a": "FAILED"})
        self.assertFalse(v.failed)
        self.assertEqual(v.newly_passing[0]["name"], "a")

    def test_new_and_vanished_tests_are_both_reported(self):
        v = ch.adjudicate([T("new", ch.PASSED)], {"gone": "PASSED"})
        self.assertEqual([t["name"] for t in v.new_tests], ["new"])
        self.assertEqual(v.missing_tests, ["gone"])
        # Neither fails the build: a harness bump adds and removes tests, and that is a review event,
        # not a 3 a.m. page.
        self.assertFalse(v.failed)

    def test_timeout_against_a_passing_baseline_is_a_regression(self):
        v = ch.adjudicate([T("a", ch.TIMEOUT)], {"a": "PASSED"})
        self.assertTrue(v.failed)
        self.assertEqual(v.regressions[0]["actual"], "TIMEOUT")

    def test_baseline_is_keyed_by_name_not_index(self):
        # Indices shift when the harness adds a test; names do not.
        b = ch.as_baseline([T("z", ch.PASSED, index=7), T("a", ch.FAILED, index=0)])
        self.assertEqual(b, {"z": "PASSED", "a": "FAILED"})


class Flake(unittest.TestCase):
    def test_a_test_that_passes_on_rerun_is_flaky_not_pass(self):
        # TEST-PLAN §0: retry transport, never adjudication. A green re-run does not delete the red
        # one, it renames it.
        self.assertEqual(ch.flake_verdict(ch.FAILED, ch.PASSED), "flaky")
        self.assertEqual(ch.flake_verdict(ch.FAILED, ch.FAILED), "fail")
        self.assertEqual(ch.flake_verdict(ch.PASSED, ch.PASSED), "pass")


class UtilRewrite(unittest.TestCase):
    SRC = ("const MEDIA_PATH = '//storage.googleapis.com/ytlr-cert.appspot.com/test-materials/media/';\n"
           "const CERT_PATH = '//storage.googleapis.com/ytlr-cert.appspot.com/test-materials/cert/';\n")

    def test_both_constants_are_repointed(self):
        out = ch.rewrite_util_js(self.SRC)
        self.assertIn(f"const MEDIA_PATH = '{ch.MEDIA_ROUTE}';", out)
        self.assertIn(f"const CERT_PATH = '{ch.CERT_ROUTE}';", out)
        self.assertNotIn("storage.googleapis.com", out)

    def test_the_vector_routes_cannot_shadow_the_harness_tree(self):
        """`/media/` DID shadow it, and the whole suite silently never started.

        js_mse_eme keeps its suite definitions in `media/` — `media/conformanceTest.js` and friends,
        loaded by main.html as ordinary script tags. Serving vectors at `/media/` sent those requests
        to Google's bucket, where they 404. No test was ever defined, `window.globalRunner` was never
        created, and cert.py reported "the harness never installed globalRunner": a message about the
        engine, for a bug in the server.
        """
        self.assertTrue(ch.MEDIA_ROUTE.startswith(ch.VECTOR_PREFIX))
        self.assertTrue(ch.CERT_ROUTE.startswith(ch.VECTOR_PREFIX))
        top = ch.VECTOR_PREFIX.strip("/")
        self.assertNotIn("/", top, "the prefix must be a single path segment")
        if not cert.HARNESS_DIR.exists():
            self.skipTest("harness not synced")
        names = {p.name for p in cert.HARNESS_DIR.iterdir()}
        self.assertNotIn(top, names,
                         f"{top!r} collides with a real entry in the harness tree")
        # And the thing that actually bit: `media` IS a real directory in there.
        self.assertIn("media", names, "harness has its own media/ — the prefix exists because of it")

    def test_a_harness_that_renamed_the_constants_raises(self):
        # The load-bearing case. A silent no-op rewrite would send every media request back to
        # Google's bucket, unhashed, and the run would still go green.
        with self.assertRaises(ch.HarnessError):
            ch.rewrite_util_js("const MEDIA_URL = '//x/';")
        with self.assertRaises(ch.HarnessError):
            ch.rewrite_util_js("const MEDIA_PATH = '//x/';")  # CERT_PATH missing

    def test_the_real_vendored_harness_still_has_the_constants(self):
        # Guards the pin: if a future HARNESS.pin bump renames these, this fails here rather than
        # silently at 3 a.m. against a bucket we do not control.
        util = cert.HARNESS_DIR / "harness" / "util.js"
        if not util.exists():
            self.skipTest("harness not synced (scripts/cert.py --sync-only)")
        out = ch.rewrite_util_js(util.read_text())
        self.assertIn(f"const MEDIA_PATH = '{ch.MEDIA_ROUTE}';", out)


class Vectors(unittest.TestCase):
    def test_a_hash_mismatch_is_env_not_a_decoder_regression(self):
        with tempfile.TemporaryDirectory() as d:
            old = cert.VECTOR_DIR
            cert.VECTOR_DIR = Path(d)
            try:
                p = Path(d) / "media" / "x.mp4"
                p.parent.mkdir(parents=True)
                p.write_bytes(b"hello")
                with self.assertRaises(cert.Env) as cm:
                    cert.fetch_vector("media", "x.mp4", {"media/x.mp4": "0" * 64}, {})
                self.assertIn("bucket changed", str(cm.exception))
            finally:
                cert.VECTOR_DIR = old

    def test_an_unknown_vector_is_recorded_not_rejected(self):
        # First run has no manifest. Record, do not guess, and do not fail.
        with tempfile.TemporaryDirectory() as d:
            old = cert.VECTOR_DIR
            cert.VECTOR_DIR = Path(d)
            try:
                p = Path(d) / "media" / "x.mp4"
                p.parent.mkdir(parents=True)
                p.write_bytes(b"hello")
                rec = {}
                cert.fetch_vector("media", "x.mp4", {}, rec)
                self.assertEqual(
                    rec["media/x.mp4"],
                    "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
                )
            finally:
                cert.VECTOR_DIR = old

    def test_manifest_round_trips(self):
        entries = {"media/a.mp4": "a" * 64, "cert/b.bin": "b" * 64}
        with tempfile.TemporaryDirectory() as d:
            old = cert.VECTOR_MANIFEST
            cert.VECTOR_MANIFEST = Path(d) / "vectors.sha256"
            try:
                cert.save_manifest(entries)
                self.assertEqual(cert.load_manifest(), entries)
            finally:
                cert.VECTOR_MANIFEST = old


class Pins(unittest.TestCase):
    HARNESS = "harness https://x/y " + "a" * 40 + " ."

    def _with_pin_file(self, text):
        d = tempfile.TemporaryDirectory()
        old = cert.PIN_FILE
        cert.PIN_FILE = Path(d.name) / "p"
        cert.PIN_FILE.write_text(text)
        return d, old

    def test_the_real_pin_file_names_three_repos(self):
        pins = cert.read_pins()
        self.assertEqual([p.name for p in pins], ["harness", "closure", "protobuf"])
        for p in pins:
            self.assertEqual(len(p.sha), 40, p.name)

    def test_a_short_or_junk_sha_is_refused(self):
        d, old = self._with_pin_file("harness https://x/y c1ef8579 .\n")
        try:
            with self.assertRaises(cert.Env):
                cert.read_pins()
        finally:
            cert.PIN_FILE = old; d.cleanup()

    def test_an_empty_pin_file_is_refused(self):
        d, old = self._with_pin_file("# only comments\n")
        try:
            with self.assertRaises(cert.Env):
                cert.read_pins()
        finally:
            cert.PIN_FILE = old; d.cleanup()

    def test_the_harness_record_must_come_first(self):
        # The others are checked out *inside* its tree; fetching them first would populate a
        # directory that the harness checkout then wipes.
        d, old = self._with_pin_file("closure https://x/y " + "b" * 40 + " sub\n" + self.HARNESS + "\n")
        try:
            with self.assertRaises(cert.Env):
                cert.read_pins()
        finally:
            cert.PIN_FILE = old; d.cleanup()

    def test_a_subdir_cannot_escape_the_harness_tree(self):
        for bad in ("/etc", "../../etc", "a/../../b"):
            d, old = self._with_pin_file(f"{self.HARNESS}\nclosure https://x/y " + "b" * 40 + f" {bad}\n")
            try:
                with self.assertRaises(cert.Env):
                    cert.read_pins()
            finally:
                cert.PIN_FILE = old; d.cleanup()


class EchoRoute(unittest.TestCase):
    """`XHRUint8Array` needs a server route the harness never mentions and never ships.

    Its absence does not present as "route missing". The XHR completes (501 fires `load`, not
    `error`), `getResponseData()` trips an internal assert, `runner.succeed()` is never reached, and
    the harness reports `XHRUint8Array TIMED OUT` — which reads as a slow or wedged engine. This test
    exists so that never happens twice.
    """

    def _serve(self):
        cert.Handler.manifest = {}
        cert.Handler.recorded = {}
        handler = functools.partial(cert.Handler, directory=str(cert.HARNESS_DIR))
        httpd = cert.Server(("127.0.0.1", 0), handler)
        t = threading.Thread(target=httpd.serve_forever, daemon=True)
        t.start()
        self.addCleanup(httpd.server_close)
        self.addCleanup(httpd.shutdown)
        return f"http://127.0.0.1:{httpd.server_address[1]}"

    def test_the_route_the_harness_hardcodes_is_the_route_we_serve(self):
        # If someone "tidies" ECHO_ROUTE, this is the tripwire: the harness's URL is a literal.
        src = (cert.HARNESS_DIR / "media" / "conformanceTest.js").read_text()
        self.assertIn(f"createPostRequest('{ch.ECHO_ROUTE}'", src)

    def test_eight_bytes_come_back_unchanged(self):
        base = self._serve()
        # BYPASS_CACHE in harness/xhr.js appends ?<epoch-ms>; the route must survive the query string.
        r = urllib.request.urlopen(urllib.request.Request(
            base + ch.ECHO_ROUTE + "?1751000000000", data=b"XHR DATA", method="POST"))
        self.assertEqual(r.status, 200)
        self.assertEqual(r.read(), b"XHR DATA")

    def test_an_oversized_body_is_refused_not_buffered(self):
        base = self._serve()
        req = urllib.request.Request(base + ch.ECHO_ROUTE, data=b"x" * 16,
                                     method="POST")
        req.add_header("Content-Length", str(cert.ECHO_MAX_BYTES + 1))
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(req, timeout=5)
        self.assertEqual(cm.exception.code, 413)

    def test_echo_is_post_only(self):
        base = self._serve()
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(base + ch.ECHO_ROUTE, timeout=5)
        self.assertEqual(cm.exception.code, 405)


class PortGuard(unittest.TestCase):
    def test_a_listening_port_is_busy(self):
        with socket.socket() as srv:
            srv.bind(("127.0.0.1", 0))
            srv.listen(1)
            self.assertFalse(cert.port_is_free(srv.getsockname()[1]))

    def test_a_free_port_is_free(self):
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        self.assertTrue(cert.port_is_free(port))

    def test_the_guard_answers_the_question_the_server_asks(self):
        """The guard must agree with `Server`, whatever `Server` does.

        It used to bind without SO_REUSEADDR while `Server.allow_reuse_address` sets it, so a
        TIME_WAIT socket from the previous run made the guard say "busy" about a port the server
        would bind fine: `just cert` twice in a row, second run dead with exit 3 and an environment
        error about nothing.
        """
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        try:
            httpd = cert.Server(("127.0.0.1", port), cert.Handler)
        except OSError:
            server_can_bind = False
        else:
            server_can_bind = True
            httpd.server_close()
        self.assertEqual(cert.port_is_free(port), server_can_bind)


class UsageExitCode(unittest.TestCase):
    def test_a_bad_flag_is_usage_not_a_conformance_regression(self):
        """argparse exits 2. In this repo 2 is EX_ASSERT, the one code that fails a build.

        `just cert conformance-test deck -- --freeze-vectors` (one `--` too many) reported itself to
        the caller as "a test that used to pass does not".
        """
        r = subprocess.run([sys.executable, "-B", str(_REPO / "scripts" / "cert.py"), "--bogus"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, cert.EX_USAGE, r.stderr)
        self.assertNotEqual(cert.EX_USAGE, cert.EX_ASSERT)


class VendoredHarness(unittest.TestCase):
    """The harness does not ship complete, and nothing in its source says so."""

    def test_every_script_source_main_html_asks_for_exists(self):
        if not (cert.HARNESS_DIR / "main.html").exists():
            self.skipTest("harness not synced (scripts/cert.py --sync-only)")
        missing = cert.missing_script_sources(cert.HARNESS_DIR)
        self.assertEqual(missing, [], f"{len(missing)} script sources missing, e.g. {missing[:2]}")

    def test_closure_is_pinned_to_a_goog_provide_release(self):
        """Closure switched `goog.array` to `goog.module` in v20200830.

        `main.html` loads all 35 Closure files as plain <script src> with CLOSURE_NO_DEPS=true, and a
        goog.module file executed as a plain script throws. Pin a newer Closure and the page fills
        with "goog.require could not find: goog.array", `window.globalRunner` is never created, and
        the failure reads exactly like a broken engine.
        """
        arr = (cert.HARNESS_DIR / "third_party/closure-library/closure-library"
                                  "/closure/goog/array/array.js")
        if not arr.exists():
            self.skipTest("closure not synced")
        head = arr.read_text()[:2000]
        self.assertIn("goog.provide('goog.array')", head)
        self.assertNotIn("goog.module('goog.array')", head)


class Tiers(unittest.TestCase):
    def test_playbackperf_is_a_trend_and_never_a_gate(self):
        # Its results depend on TDP, refresh rate, and thermals. On a handheld it is not
        # deterministic, and a nondeterministic gate is a gate someone disables.
        self.assertFalse(set(cert.GATE_SUITES) & set(cert.TREND_SUITES))
        for s in cert.TREND_SUITES:
            self.assertIn("playbackperf", s)

    def test_widevine_suites_are_not_in_the_automated_gate(self):
        # CI must never fetch Google's CDM (docs/legal.md), so the unattended run is ClearKey only.
        for s in cert.GATE_SUITES + cert.TREND_SUITES:
            self.assertNotIn("widevine", s)

    def test_no_baseline_means_no_pass(self):
        old = cert.EXPECT_DIR
        cert.EXPECT_DIR = Path("/nonexistent-baseline-dir")
        try:
            with self.assertRaises(cert.Env) as cm:
                cert.load_baseline("conformance-test")
            self.assertIn("--update-baseline", str(cm.exception))
        finally:
            cert.EXPECT_DIR = old


class JUnit(unittest.TestCase):
    def test_regressions_are_failures_and_unknowns_are_errors(self):
        tests = [T("ok", ch.PASSED), T("bad", ch.FAILED), T("never", ch.UNKNOWN)]
        v = ch.adjudicate(tests, {"ok": "PASSED", "bad": "PASSED", "never": "PASSED"})
        xml = ch.junit_xml("conformance-test", tests, v, {"harness_commit": "abc"})
        self.assertIn('failures="1"', xml)
        self.assertIn('errors="1"', xml)
        self.assertIn("<failure", xml)
        self.assertIn("<error", xml)
        self.assertIn('name="harness_commit"', xml)

    def test_a_known_failure_is_recorded_but_is_not_a_junit_failure(self):
        tests = [T("opt", ch.OPTIONAL_FAILED)]
        v = ch.adjudicate(tests, {"opt": "OPTIONAL_FAILED"})
        xml = ch.junit_xml("s", tests, v, None)
        self.assertIn('failures="0"', xml)
        self.assertIn("OPTIONAL_FAILED (expected, per baseline)", xml)


if __name__ == "__main__":
    unittest.main(verbosity=2)
