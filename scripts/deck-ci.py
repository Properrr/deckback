#!/usr/bin/env python3
"""Unattended on-Deck run: deploy, launch, adjudicate, tear down, report. No human in the loop.

    scripts/deck-ci.py                     # deploy, launch, gate, cert   (the regression subset)
    scripts/deck-ci.py --full              # + probe, power, soak
    scripts/deck-ci.py --stages launch,gate
    scripts/deck-ci.py --dry-run           # print the plan and the exit-code policy; touch nothing

Runs on the WORKSTATION. The Deck is a dumb target reached over SSH; nothing is installed there by
this script beyond what `just deploy` already puts in `~/cobalt-yt/`.

Exit codes (.internal/HARNESS.md §1) are aggregated, not concatenated:

    2  ASSERT     some gate says the product is wrong. Outranks everything below it: a regression
                  must not be masked by an SSH drop in a later stage.
    4  TRANSPORT  the wire broke and retries did not fix it. Retry the run.
    3  ENV        a precondition was missing — OR NOTHING ADJUDICATED. A run that deployed, launched
                  and then skipped every test proves nothing, and 0 would be a lie.
    1  FAIL       something else went wrong.
    0  ok         at least one adjudicating stage ran and passed, and nothing that matters failed.

Every decision above is `scripts/lib/deckci.py`, which is pure and covered by
`tests/harness/test_deckci.py`. This file only runs processes and writes files, because this file is
the part that cannot be tested without a Deck.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_REPO / "scripts" / "lib"))

import deckci  # noqa: E402
from exitcodes import Parser  # noqa: E402

ARTIFACTS = _REPO / "artifacts" / "deck-ci"


def _say(msg):
    print(msg, file=sys.stderr, flush=True)


def build_stages(args):
    """The stage table. Order here IS execution order (see deckci.plan)."""
    sh = str(_REPO / "scripts")
    launch = "remote-run.sh" if args.launch == "steam" else "run.sh"
    return [
        deckci.Stage(
            "deploy", [f"{sh}/deploy.sh"], blocking=True, timeout=900,
            why="rsync the stripped build to ~/cobalt-yt/. Nothing downstream means anything "
                "without it."),
        deckci.Stage(
            "launch", [f"{sh}/{launch}"], blocking=True, timeout=180,
            why="start the app and WAIT for its CDP endpoint. remote-run.sh already refuses to "
                "report success for a fire-and-forget Steam URI."),
        deckci.Stage(
            "gate", [f"{sh}/test-deck.sh", "-m", "gate", "--no-skip"],
            adjudicates=True, timeout=1800,
            why="the regression subset: every L2 test whose failure is a product defect. --no-skip "
                "because for an unattended runner a missing Deck IS the bug."),
        deckci.Stage(
            "cert", [f"{sh}/cert.sh", "--deck", "conformance-test"],
            adjudicates=True, timeout=2400,
            why="L3 conformance against real hardware decode. Fails only on a regression against "
                "tests/cert/expectations/."),
        deckci.Stage(
            "probe", [f"{sh}/test-deck.sh", "-m", "probe", "--no-skip"], timeout=1800,
            why="discovery tests for still-unverified behaviour. A failure is a FINDING to register, "
                "not a regression — reported, never silently swallowed, never fails the build."),
        deckci.Stage(
            "power", [f"{sh}/power.sh", str(args.power_seconds)], adjudicates=True, timeout=900,
            why="the P4 battery gate. Refuses to sample unless a video is really playing on the "
                "VA-API decoder, so 'no video' arrives here as ENV (3), not as a pass."),
        deckci.Stage(
            "soak", [f"{sh}/soak.sh", str(args.soak_cycles)], adjudicates=True, timeout=5400,
            why="P6 suspend/resume. Requires currentTime to advance after every resume."),
    ]


DEFAULT = ["deploy", "launch", "gate", "cert"]
FULL = ["deploy", "launch", "gate", "cert", "probe", "power", "soak"]


def run_stage(stage, logdir, attempts):
    """Run one stage, retrying only on TRANSPORT. Returns a deckci.Result."""
    log = logdir / f"{stage.name}.log"
    attempt = 0
    started = time.monotonic()
    rc, note = deckci.EX_FAIL, ""
    while True:
        attempt += 1
        _say(f"deck-ci: {stage.name} (attempt {attempt}/{attempts}) ...")
        with log.open("ab") as fh:
            fh.write(f"\n=== attempt {attempt}: {' '.join(stage.argv)} ===\n".encode())
            fh.flush()
            try:
                rc = subprocess.call(stage.argv, cwd=_REPO, stdout=fh,
                                     stderr=subprocess.STDOUT, timeout=stage.timeout)
            except subprocess.TimeoutExpired:
                # A stage that never returns is not a passing stage, and it is not the wire's fault
                # either -- retrying a hang just doubles the wait. Call it what it is.
                rc = deckci.EX_FAIL
                note = f"timed out after {stage.timeout}s"
                fh.write(f"\n!! timed out after {stage.timeout}s\n".encode())
            except FileNotFoundError:
                rc, note = deckci.EX_ENV, f"{stage.argv[0]} not found"
        outcome = deckci.classify(rc)
        if not deckci.should_retry(outcome, attempt, attempts):
            break
        _say(f"deck-ci: {stage.name} hit TRANSPORT (rc={rc}); retrying")
        time.sleep(min(5 * attempt, 20))

    return deckci.Result(stage, rc, attempts=attempt, seconds=time.monotonic() - started, note=note)


def teardown(logdir):
    """Always attempted, never able to change the verdict — and never silent.

    Leaving the app running on a Deck poisons the next run: `just power` would sample a video that
    the previous run started and report a number about the wrong thing.

    Goes through `deck_ssh` in lib.sh rather than calling `ssh` directly, because that helper is
    where DECK_PORT and the optional sshpass live. A hand-rolled `ssh $DECK_HOST` here would silently
    connect to the wrong port on any Deck that is not on 22.
    """
    host = os.environ.get("DECK_HOST", "")
    if not host:
        # This is the failure mode the export in deck-ci.sh exists to prevent. Say it out loud.
        _say("deck-ci: teardown SKIPPED — DECK_HOST is not in the environment. If you invoked "
             "deck-ci.py directly, use scripts/deck-ci.sh (or `just deck-ci`); the app may still be "
             "running on the Deck.")
        return
    script = ('. scripts/lib.sh; deck_ssh -o BatchMode=yes -o ConnectTimeout=5 "$DECK_HOST" '
              "'flatpak kill io.github.properrr.deckback 2>/dev/null; "
              "pkill -f deckback-launcher 2>/dev/null; true'")
    with (logdir / "teardown.log").open("wb") as fh:
        rc = subprocess.call(["bash", "-c", script], cwd=_REPO, stdout=fh,
                             stderr=subprocess.STDOUT, timeout=60)
    _say(f"deck-ci: teardown {'ok' if rc == 0 else f'FAILED (rc={rc}); the app may still be running'}")


def main():
    # Parser, not ArgumentParser: a mistyped flag must exit 5 (usage), never 2 (the product is
    # wrong). This runner is the thing that decides whether to page someone.
    ap = Parser(description=__doc__.splitlines()[0],
                formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stages", help=f"comma-separated. Default: {','.join(DEFAULT)}")
    ap.add_argument("--full", action="store_true", help=f"run {','.join(FULL)}")
    ap.add_argument("--launch", choices=("ssh", "steam"), default="steam",
                    help="steam: the installed Flatpak via a Game Mode shortcut (what users get). "
                         "ssh: the deployed build directly (no Steam, no Flatpak).")
    ap.add_argument("--retries", type=int, default=2,
                    help="attempts per stage on TRANSPORT only. Never on a test result.")
    ap.add_argument("--power-seconds", type=int, default=180)
    ap.add_argument("--soak-cycles", type=int, default=10)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--stamp", default=None,
                    help="artifact directory name (default: UTC timestamp)")
    args = ap.parse_args()

    if args.stages and args.full:
        _say("deck-ci: --stages and --full are mutually exclusive")
        return deckci.EX_USAGE
    if args.retries < 1:
        _say("deck-ci: --retries must be >= 1")
        return deckci.EX_USAGE

    selected = FULL if args.full else (args.stages.split(",") if args.stages else DEFAULT)
    try:
        stages = deckci.plan(build_stages(args), [s.strip() for s in selected if s.strip()])
    except ValueError as e:
        _say(f"deck-ci: {e}")
        return deckci.EX_USAGE
    if not stages:
        _say("deck-ci: no stages selected")
        return deckci.EX_USAGE

    if args.dry_run:
        print(f"plan ({len(stages)} stages, order is fixed, retries={args.retries} on TRANSPORT only):\n")
        for s in stages:
            tags = [t for t, on in (("blocking", s.blocking), ("adjudicates", s.adjudicates)) if on]
            print(f"  {s.name:<7} {'[' + ','.join(tags) + ']' if tags else ''}")
            print(f"          $ {' '.join(s.argv)}")
            print(f"          {s.why}\n")
        adjudicating = [s.name for s in stages if s.adjudicates]
        print(f"  adjudicating stages: {', '.join(adjudicating) or 'NONE'}")
        if not adjudicating:
            print("  => this plan can never exit 0: it would prove nothing about the product.")
        return deckci.EX_OK

    stamp = args.stamp or time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    logdir = ARTIFACTS / stamp
    logdir.mkdir(parents=True, exist_ok=True)
    _say(f"deck-ci: {len(stages)} stage(s), artifacts -> {logdir.relative_to(_REPO)}")

    results, halted = [], None
    try:
        for stage in stages:
            r = run_stage(stage, logdir, args.retries)
            results.append(r)
            _say(f"deck-ci: {stage.name} -> {r.outcome} (rc={r.rc}, {r.seconds:.1f}s)")
            if deckci.halts_after(r):
                halted = stage.name
                _say(f"deck-ci: {stage.name} is blocking and did not pass — skipping the rest, "
                     "because everything after it would be measuring a machine in an unknown state.")
                break
    finally:
        teardown(logdir)

    v = deckci.verdict(results)
    meta = {"stamp": stamp, "host": os.environ.get("DECK_HOST", ""), "launch": args.launch,
            "retries": args.retries, "halted_after": halted,
            "planned": [s.name for s in stages]}
    (logdir / "summary.json").write_text(json.dumps(deckci.as_json(v, meta), indent=2) + "\n")

    print("\ndeck-ci summary")
    print(deckci.summarize(v))
    for r in results:
        if not r.ok and not r.stage.adjudicates and not r.stage.blocking:
            print(f"\n  note: {r.stage.name} exited {r.rc} ({r.outcome}) and did NOT fail the build.\n"
                  f"        {r.stage.why}\n"
                  f"        Read {(logdir / (r.stage.name + '.log')).relative_to(_REPO)} — a probe "
                  "failure is a finding to register.")
    print(f"\n  artifacts: {logdir.relative_to(_REPO)}")
    return v.exit_code


if __name__ == "__main__":
    sys.exit(main())
