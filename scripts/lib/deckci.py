"""Pure decision logic for the unattended on-Deck runner (`scripts/deck-ci.py`).

Nothing here touches SSH, a Deck, or a subprocess. That is the point: the runner's *judgement* — what
to run, what to retry, what a mixed bag of exit codes means — is the part that must be right at 3
a.m., and it is the part no one can test with hardware in the loop. F7 said the L2 harness must be
testable without the hardware it tests. This is the same rule applied to the thing that drives it.

Three rules, all of them learned the hard way, all of them encoded here rather than in a comment:

  * **Retry transport, never adjudication** (TEST-PLAN §0). A dropped SSH connection is noise. A
    failing test is a fact, and re-running it until it passes is how a suite stops meaning anything.
  * **A run that adjudicated nothing did not pass.** `just power` once reported `mean 0.00 W … PASS`
    on a Deck with no battery telemetry (F1); `just test-deck` reports success when all 23 tests skip.
    A verdict computed from tests that did not happen is not a verdict. `verdict()` refuses to return
    0 unless an adjudicating stage actually ran and passed.
  * **A regression outranks the noise that follows it.** If `gate` exits 2 and a later stage then
    loses the SSH connection, the run is a 2. Reporting 4 would invite a retry that buries the defect.
"""

from __future__ import annotations

from exitcodes import EX_OK, EX_FAIL, EX_ASSERT, EX_ENV, EX_TRANSPORT, EX_USAGE  # noqa: F401

OK, FAIL, ASSERT, ENV, TRANSPORT, USAGE = "ok", "fail", "assert", "env", "transport", "usage"

_BY_CODE = {EX_OK: OK, EX_FAIL: FAIL, EX_ASSERT: ASSERT, EX_ENV: ENV,
            EX_TRANSPORT: TRANSPORT, EX_USAGE: USAGE}


def classify(rc):
    """Exit code -> outcome name. Anything unmapped is a generic failure, never a pass.

    An unknown code is the one case where guessing is dangerous in exactly one direction, so it
    guesses in the other.
    """
    return _BY_CODE.get(rc, FAIL)


class Stage:
    """One step of the run.

    blocking      nothing after it is meaningful if it does not succeed (deploy, launch)
    adjudicates   its verdict is *about the product*; a green run needs at least one of these
    retry_on      outcomes worth another attempt. Only ever TRANSPORT.
    """

    __slots__ = ("name", "argv", "blocking", "adjudicates", "why", "timeout")

    def __init__(self, name, argv, *, blocking=False, adjudicates=False, why="", timeout=None):
        self.name, self.argv = name, argv
        self.blocking, self.adjudicates = blocking, adjudicates
        self.why, self.timeout = why, timeout

    def __repr__(self):
        return f"<Stage {self.name}>"


def retryable(outcome):
    """Only the wire. Never a test result, never a broken environment.

    ENV is deliberately not retryable: a missing Steam shortcut or an absent battery node will still
    be missing in ten seconds, and retrying it just delays the report by ten seconds.
    """
    return outcome == TRANSPORT


def should_retry(outcome, attempt, max_attempts):
    return retryable(outcome) and attempt < max_attempts


class Result:
    __slots__ = ("stage", "rc", "outcome", "attempts", "seconds", "note")

    def __init__(self, stage, rc, attempts=1, seconds=0.0, note=""):
        self.stage, self.rc = stage, rc
        self.outcome = classify(rc)
        self.attempts, self.seconds, self.note = attempts, seconds, note

    @property
    def ok(self):
        return self.outcome == OK

    def as_dict(self):
        return {"stage": self.stage.name, "rc": self.rc, "outcome": self.outcome,
                "attempts": self.attempts, "seconds": round(self.seconds, 1),
                "adjudicates": self.stage.adjudicates, "note": self.note}


class Verdict:
    __slots__ = ("exit_code", "reason", "results")

    def __init__(self, exit_code, reason, results):
        self.exit_code, self.reason, self.results = exit_code, reason, results

    @property
    def passed(self):
        return self.exit_code == EX_OK


# Precedence when several stages went wrong at once. A regression is the most actionable thing that
# can happen, and it must not be masked by an SSH drop in a later stage.
_PRECEDENCE = [ASSERT, TRANSPORT, ENV, USAGE, FAIL]


def verdict(results):
    """Turn a list of Results into one exit code, and say why.

    The interesting case is not "something failed" — it is "nothing failed, and nothing happened".
    """
    if not results:
        return Verdict(EX_ENV, "no stages ran at all", results)

    # A non-adjudicating stage (probe, trend) may fail without failing the build, but it is never
    # silently swallowed: it is reported and its outcome is in the JSON. Same posture as
    # `playbackperf-*` in cert.py, and same reason — a gate that fires randomly is a gate someone
    # turns off, and then it stops reporting too.
    judging = [r for r in results if r.stage.adjudicates]
    bad = [r for r in results if not r.ok and (r.stage.adjudicates or r.stage.blocking)]
    if bad:
        for outcome in _PRECEDENCE:
            hit = [r for r in bad if r.outcome == outcome]
            if hit:
                names = ", ".join(r.stage.name for r in hit)
                code = next(c for c, n in _BY_CODE.items() if n == outcome)
                return Verdict(code, f"{outcome}: {names}", results)

    # Everything that ran, passed. Did anything *adjudicate*?
    #
    # This is F1's rule, generalised. A run that deployed, launched, and then skipped every test
    # because the Deck went away has proved nothing about the product, and answering 0 to "did it
    # pass?" is a lie that CI will happily believe forever.
    if not judging:
        return Verdict(EX_ENV, "no adjudicating stage ran — this run proves nothing", results)
    return Verdict(EX_OK, f"{len(judging)} adjudicating stage(s) passed", results)


def plan(stages, selected):
    """Order the requested stages canonically; unknown names are a USAGE error.

    Stages always run in the order they are *defined*, never in the order they are typed: `--stages
    gate,launch` must not try to test an app that has not been launched.
    """
    known = {s.name: s for s in stages}
    unknown = [n for n in selected if n not in known]
    if unknown:
        raise ValueError(f"unknown stage(s): {', '.join(unknown)}. "
                         f"Known: {', '.join(known)}")
    return [s for s in stages if s.name in set(selected)]


def halts_after(result):
    """After a blocking stage fails, everything downstream is measuring a machine in an unknown
    state. Stop, and say so, rather than emitting a wall of red that all has one cause."""
    return result.stage.blocking and not result.ok


def summarize(verdict_):
    """A human-readable block. Adjudicating stages are marked, because 'FAIL' on a probe and 'FAIL'
    on a gate are different sentences."""
    lines = []
    width = max((len(r.stage.name) for r in verdict_.results), default=8)
    for r in verdict_.results:
        kind = "gate " if r.stage.adjudicates else "     "
        retry = f"  ({r.attempts} attempts)" if r.attempts > 1 else ""
        lines.append(f"  {kind}{r.stage.name:<{width}}  {r.outcome:<9} rc={r.rc}  "
                     f"{r.seconds:6.1f}s{retry}")
    lines.append("")
    lines.append(f"  => exit {verdict_.exit_code} ({classify(verdict_.exit_code)}): {verdict_.reason}")
    return "\n".join(lines)


def as_json(verdict_, meta):
    return {"meta": meta,
            "exit_code": verdict_.exit_code,
            "outcome": classify(verdict_.exit_code),
            "reason": verdict_.reason,
            "stages": [r.as_dict() for r in verdict_.results]}
