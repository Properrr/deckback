#!/usr/bin/env python3
"""L0 coverage of the unattended on-Deck runner's judgement (`scripts/lib/deckci.py`).

There is no Deck here, and there will not be one in CI. So the runner is split in two: a process
driver that cannot be tested without hardware, and the decisions — what to retry, when to stop, what
a mixed bag of exit codes means — which can be tested completely, and are the only part that can be
wrong in a way nobody notices.

The test that matters most is `test_a_run_that_adjudicated_nothing_is_not_a_pass`. Every other bug in
this file makes CI red for the wrong reason. That one makes it green for no reason.
"""

import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

_HERE = Path(os.path.dirname(os.path.abspath(__file__)))
_REPO = _HERE.parent.parent
sys.path.insert(0, str(_REPO / "scripts" / "lib"))

import deckci  # noqa: E402


def S(name, **kw):
    return deckci.Stage(name, ["/bin/true"], **kw)


def R(stage, rc, **kw):
    return deckci.Result(stage, rc, **kw)


DEPLOY = S("deploy", blocking=True)
LAUNCH = S("launch", blocking=True)
GATE = S("gate", adjudicates=True)
CERT = S("cert", adjudicates=True)
PROBE = S("probe")
POWER = S("power", adjudicates=True)


class Classify(unittest.TestCase):
    def test_the_codes_are_the_harness_taxonomy(self):
        self.assertEqual(
            (deckci.EX_OK, deckci.EX_FAIL, deckci.EX_ASSERT, deckci.EX_ENV,
             deckci.EX_TRANSPORT, deckci.EX_USAGE), (0, 1, 2, 3, 4, 5))

    def test_they_agree_with_scripts_lib_sh(self):
        """Two copies of the taxonomy exist; they must not drift. `deck-ci.py` aggregates exit codes
        produced by shell scripts, so a mismatch would silently reclassify a regression."""
        text = (_REPO / "scripts" / "lib.sh").read_text()
        m = re.search(r"readonly EX_OK=(\d) EX_FAIL=(\d) EX_ASSERT=(\d) EX_ENV=(\d) "
                      r"EX_TRANSPORT=(\d) EX_USAGE=(\d)", text)
        self.assertIsNotNone(m, "scripts/lib.sh no longer declares the taxonomy where we look")
        self.assertEqual([int(g) for g in m.groups()], [0, 1, 2, 3, 4, 5])

    def test_an_unknown_exit_code_is_a_failure_not_a_pass(self):
        # The one place guessing is dangerous in exactly one direction.
        self.assertEqual(deckci.classify(137), deckci.FAIL)   # SIGKILL via shell
        self.assertEqual(deckci.classify(-9), deckci.FAIL)
        self.assertNotEqual(deckci.classify(99), deckci.OK)


class Retry(unittest.TestCase):
    def test_only_transport_is_retried(self):
        self.assertTrue(deckci.retryable(deckci.TRANSPORT))
        for outcome in (deckci.OK, deckci.FAIL, deckci.ASSERT, deckci.ENV, deckci.USAGE):
            self.assertFalse(deckci.retryable(outcome), outcome)

    def test_a_failing_test_is_never_retried(self):
        """TEST-PLAN §0: retry transport, never adjudication. Re-running a red test until it goes
        green is how a suite stops meaning anything."""
        self.assertFalse(deckci.should_retry(deckci.ASSERT, attempt=1, max_attempts=5))

    def test_env_is_not_retried_either(self):
        # A missing Steam shortcut will still be missing in ten seconds.
        self.assertFalse(deckci.should_retry(deckci.ENV, attempt=1, max_attempts=5))

    def test_retries_are_bounded(self):
        self.assertTrue(deckci.should_retry(deckci.TRANSPORT, attempt=1, max_attempts=2))
        self.assertFalse(deckci.should_retry(deckci.TRANSPORT, attempt=2, max_attempts=2))


class TheVerdict(unittest.TestCase):
    def test_a_clean_run_passes(self):
        v = deckci.verdict([R(DEPLOY, 0), R(LAUNCH, 0), R(GATE, 0), R(CERT, 0)])
        self.assertEqual(v.exit_code, deckci.EX_OK)
        self.assertTrue(v.passed)

    def test_a_run_that_adjudicated_nothing_is_not_a_pass(self):
        """THE test this module exists for.

        Deploy worked. Launch worked. No gate ran. Every stage that ran, passed — and the run has no
        opinion whatsoever about the product. `just power` once answered this question with
        `mean 0.00 W ... PASS` (F1); `just test-deck` answers it with 23 skips and exit 0.
        """
        v = deckci.verdict([R(DEPLOY, 0), R(LAUNCH, 0)])
        self.assertEqual(v.exit_code, deckci.EX_ENV)
        self.assertIn("proves nothing", v.reason)

    def test_no_stages_at_all_is_not_a_pass(self):
        v = deckci.verdict([])
        self.assertEqual(v.exit_code, deckci.EX_ENV)
        # Pin the REASON, not just the code. "nothing ran" and "nothing judged" both exit 3, so an
        # assertion on the code alone cannot tell whether the empty-run branch exists at all — the
        # mutation that deletes it survives, and the operator loses the one sentence that says which
        # of the two happened.
        self.assertIn("no stages ran", v.reason)

    def test_a_gate_that_skipped_the_whole_suite_is_env_not_ok(self):
        # test-deck.sh --no-skip turns "no Deck" into an error; if it ever exits 3 anyway, the run
        # must not be green.
        v = deckci.verdict([R(DEPLOY, 0), R(LAUNCH, 0), R(GATE, deckci.EX_ENV)])
        self.assertEqual(v.exit_code, deckci.EX_ENV)

    def test_a_regression_outranks_a_later_transport_drop(self):
        """If `gate` says the product is wrong and then SSH dies during `cert`, the answer is 2.
        Answering 4 invites a retry, and the retry buries the defect."""
        v = deckci.verdict([R(GATE, deckci.EX_ASSERT), R(CERT, deckci.EX_TRANSPORT)])
        self.assertEqual(v.exit_code, deckci.EX_ASSERT)
        self.assertIn("gate", v.reason)

    def test_a_regression_outranks_a_later_env(self):
        v = deckci.verdict([R(GATE, deckci.EX_ASSERT), R(POWER, deckci.EX_ENV)])
        self.assertEqual(v.exit_code, deckci.EX_ASSERT)

    def test_transport_outranks_env(self):
        v = deckci.verdict([R(GATE, deckci.EX_TRANSPORT), R(CERT, deckci.EX_ENV)])
        self.assertEqual(v.exit_code, deckci.EX_TRANSPORT)

    def test_a_probe_failure_is_reported_but_never_fails_the_build(self):
        """conftest.py: 'probe: a failure is a FINDING to register, not a regression — but it is
        still reported, never silently swallowed.'"""
        v = deckci.verdict([R(GATE, 0), R(PROBE, deckci.EX_ASSERT)])
        self.assertEqual(v.exit_code, deckci.EX_OK)
        probe = [r for r in v.results if r.stage.name == "probe"][0]
        self.assertFalse(probe.ok)
        self.assertEqual(probe.outcome, deckci.ASSERT)  # visible in summary.json

    def test_a_probe_cannot_rescue_a_run_that_adjudicated_nothing(self):
        # A probe passing is not evidence about the product.
        v = deckci.verdict([R(DEPLOY, 0), R(PROBE, 0)])
        self.assertEqual(v.exit_code, deckci.EX_ENV)

    def test_a_blocking_stage_failing_fails_the_run(self):
        for rc, expect in ((deckci.EX_TRANSPORT, deckci.EX_TRANSPORT),
                           (deckci.EX_ENV, deckci.EX_ENV),
                           (deckci.EX_ASSERT, deckci.EX_ASSERT),
                           (deckci.EX_FAIL, deckci.EX_FAIL)):
            v = deckci.verdict([R(DEPLOY, rc)])
            self.assertEqual(v.exit_code, expect, f"deploy rc={rc}")

    def test_power_reporting_env_because_nothing_was_playing_is_not_a_pass(self):
        # power.sh refuses to sample a paused video. That refusal must not read as success.
        v = deckci.verdict([R(GATE, 0), R(POWER, deckci.EX_ENV)])
        self.assertEqual(v.exit_code, deckci.EX_ENV)

    def test_the_reason_names_the_stage(self):
        v = deckci.verdict([R(GATE, 0), R(CERT, deckci.EX_ASSERT)])
        self.assertIn("cert", v.reason)


class Halting(unittest.TestCase):
    def test_a_failed_blocking_stage_halts_the_run(self):
        self.assertTrue(deckci.halts_after(R(LAUNCH, deckci.EX_ASSERT)))

    def test_a_failed_gate_does_not_halt_the_run(self):
        # cert still has something useful to say about a build whose gate went red.
        self.assertFalse(deckci.halts_after(R(GATE, deckci.EX_ASSERT)))

    def test_a_passing_blocking_stage_does_not_halt(self):
        self.assertFalse(deckci.halts_after(R(DEPLOY, 0)))


class Plan(unittest.TestCase):
    STAGES = [DEPLOY, LAUNCH, GATE, CERT, PROBE, POWER]

    def test_order_is_definition_order_not_argument_order(self):
        """`--stages gate,launch` must not test an app that has not been launched."""
        got = [s.name for s in deckci.plan(self.STAGES, ["gate", "launch"])]
        self.assertEqual(got, ["launch", "gate"])

    def test_an_unknown_stage_is_refused(self):
        with self.assertRaises(ValueError) as cm:
            deckci.plan(self.STAGES, ["gate", "gaet"])
        self.assertIn("gaet", str(cm.exception))

    def test_duplicates_collapse(self):
        got = [s.name for s in deckci.plan(self.STAGES, ["gate", "gate"])]
        self.assertEqual(got, ["gate"])

    def test_an_empty_selection_selects_nothing(self):
        self.assertEqual(deckci.plan(self.STAGES, []), [])


class TheRunnerScript(unittest.TestCase):
    """Two things about scripts/deck-ci.{sh,py} that no amount of pure-logic testing would catch."""

    def test_the_shim_exports_deck_host_so_teardown_is_not_a_no_op(self):
        """lib.sh SETS DECK_HOST; it does not export it.

        Without the export, `deck-ci.py`'s teardown reads DECK_HOST from an environment that does not
        have it, returns immediately, and leaves the app running on the Deck — where the next
        `just power` samples a video the previous run started. A cleanup step that silently no-ops is
        worse than none, because you stop looking for the mess.
        """
        text = (_REPO / "scripts" / "deck-ci.sh").read_text()
        # re.M: the line is not at the start of the file. Without it this passed only because
        # assertRegex searches, and it searched for something that could never match.
        self.assertIsNotNone(re.search(r"^export DECK_HOST DECK_PORT\b", text, re.M),
                             "scripts/deck-ci.sh must export DECK_HOST (lib.sh only sets it)")

    def test_teardown_goes_through_deck_ssh(self):
        """`ssh $DECK_HOST` would ignore DECK_PORT and the optional sshpass, and would silently
        connect to the wrong port on any Deck that is not on 22."""
        text = (_REPO / "scripts" / "deck-ci.py").read_text()
        self.assertIn("deck_ssh", text)

    def test_a_mistyped_flag_is_usage_not_a_regression(self):
        """argparse exits 2, and 2 is EX_ASSERT — the one code that fails a build. This runner is the
        thing that decides whether to page someone at 3 a.m."""
        r = subprocess.run([sys.executable, "-B", str(_REPO / "scripts" / "deck-ci.py"), "--bogus"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, deckci.EX_USAGE, r.stderr)

    def test_a_plan_with_no_adjudicating_stage_says_so_up_front(self):
        r = subprocess.run([sys.executable, "-B", str(_REPO / "scripts" / "deck-ci.py"),
                            "--dry-run", "--stages", "deploy,launch"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0)
        self.assertIn("can never exit 0", r.stdout)

    def test_the_default_plan_adjudicates_something(self):
        r = subprocess.run([sys.executable, "-B", str(_REPO / "scripts" / "deck-ci.py"), "--dry-run"],
                           capture_output=True, text=True)
        self.assertNotIn("can never exit 0", r.stdout)
        self.assertIn("adjudicating stages: gate, cert", r.stdout)


class Reporting(unittest.TestCase):
    def test_the_summary_distinguishes_a_gate_from_a_probe(self):
        v = deckci.verdict([R(GATE, 0), R(PROBE, deckci.EX_ASSERT)])
        text = deckci.summarize(v)
        gate_line = [l for l in text.splitlines() if "gate" in l][0]
        probe_line = [l for l in text.splitlines() if "probe" in l][0]
        self.assertIn("gate ", gate_line)
        self.assertNotIn("gate ", probe_line.replace("probe", ""))

    def test_the_summary_shows_the_exit_code_and_the_reason(self):
        v = deckci.verdict([R(GATE, deckci.EX_ASSERT)])
        text = deckci.summarize(v)
        self.assertIn("exit 2", text)
        self.assertIn("assert", text)

    def test_retries_are_visible(self):
        v = deckci.verdict([R(GATE, 0, attempts=3)])
        self.assertIn("3 attempts", deckci.summarize(v))

    def test_json_is_machine_readable_and_names_the_adjudicators(self):
        v = deckci.verdict([R(GATE, 0), R(PROBE, deckci.EX_ASSERT)])
        blob = deckci.as_json(v, {"host": "deck"})
        self.assertEqual(blob["exit_code"], 0)
        self.assertEqual(blob["outcome"], "ok")
        by = {s["stage"]: s for s in blob["stages"]}
        self.assertTrue(by["gate"]["adjudicates"])
        self.assertFalse(by["probe"]["adjudicates"])
        self.assertEqual(by["probe"]["outcome"], "assert")


if __name__ == "__main__":
    unittest.main(verbosity=2)
