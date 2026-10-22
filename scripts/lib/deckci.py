"""Pure decision logic (no SSH, no subprocess) for the unattended on-Deck runner `scripts/deck-ci.py`.

  * Retry transport, never adjudication.
  * A run in which no adjudicating stage ran and passed exits ENV, never 0.
  * A regression outranks later noise: `gate` exiting 2 then an SSH drop is a 2, not a 4.
"""

from __future__ import annotations

from exitcodes import EX_OK, EX_FAIL, EX_ASSERT, EX_ENV, EX_TRANSPORT, EX_USAGE  # noqa: F401

OK, FAIL, ASSERT, ENV, TRANSPORT, USAGE = "ok", "fail", "assert", "env", "transport", "usage"

_BY_CODE = {EX_OK: OK, EX_FAIL: FAIL, EX_ASSERT: ASSERT, EX_ENV: ENV,
            EX_TRANSPORT: TRANSPORT, EX_USAGE: USAGE}


def classify(rc):
    """Exit code -> outcome name. Anything unmapped is a generic failure, never a pass."""
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
    """Only TRANSPORT is retryable; never a test result or a broken environment."""
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


# Precedence when several stages went wrong at once.
_PRECEDENCE = [ASSERT, TRANSPORT, ENV, USAGE, FAIL]


def verdict(results):
    """Turn a list of Results into one exit code and a reason."""
    if not results:
        return Verdict(EX_ENV, "no stages ran at all", results)

    # A failing non-adjudicating, non-blocking stage (probe, trend) is reported but does not fail.
    judging = [r for r in results if r.stage.adjudicates]
    bad = [r for r in results if not r.ok and (r.stage.adjudicates or r.stage.blocking)]
    if bad:
        for outcome in _PRECEDENCE:
            hit = [r for r in bad if r.outcome == outcome]
            if hit:
                names = ", ".join(r.stage.name for r in hit)
                code = next(c for c, n in _BY_CODE.items() if n == outcome)
                return Verdict(code, f"{outcome}: {names}", results)

    if not judging:
        return Verdict(EX_ENV, "no adjudicating stage ran — this run proves nothing", results)
    return Verdict(EX_OK, f"{len(judging)} adjudicating stage(s) passed", results)


def plan(stages, selected):
    """The selected stages in definition order, not typed order; unknown names raise ValueError."""
    known = {s.name: s for s in stages}
    unknown = [n for n in selected if n not in known]
    if unknown:
        raise ValueError(f"unknown stage(s): {', '.join(unknown)}. "
                         f"Known: {', '.join(known)}")
    return [s for s in stages if s.name in set(selected)]


def halts_after(result):
    """True when a blocking stage failed, so nothing downstream should run."""
    return result.stage.blocking and not result.ok


def summarize(verdict_):
    """A human-readable block; adjudicating stages are marked `gate`."""
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
