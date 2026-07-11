"""L2 fixtures: SSH to the Deck, a CDP tunnel, and artifacts on failure (TEST-PLAN §3).

Topology: the workstation is the test runner, the Deck is a dumb target. Nothing is installed there.

A word about skipping. With no Deck reachable this whole suite skips, and **a suite that skips proves
nothing**. That is acceptable only because the pure helpers under `lib/` are separately unit-tested
by `tests/harness/test_deck_lib.py`, which CI *does* run. Never let a skip stand in for a pass: `just
test-deck` prints how many tests ran, and zero is not success.
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parent.parent
sys.path.insert(0, str(_HERE))
sys.path.insert(0, str(_REPO / "scripts"))

from lib import ssh as sshlib  # noqa: E402

CDP_PORT = int(os.environ.get("DECKBACK_CDP_PORT", "9222"))
ARTIFACTS = _REPO / "artifacts"


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "gate: must pass. A failure is a product defect and blocks the release.",
    )
    config.addinivalue_line(
        "markers",
        "probe: a discovery test for something unverified. A failure is a FINDING to register, "
        "not a regression — but it is still reported, never silently swallowed.",
    )
    config.addinivalue_line(
        "markers", "uinput: needs /dev/uinput on the Deck (udev rule; see lib/uinput.py)."
    )
    config.addinivalue_line("markers", "playback: needs a video playing.")


def pytest_addoption(parser):
    parser.addoption("--deck-host", action="store", default=None, help="override DECK_HOST")
    parser.addoption(
        "--no-skip",
        action="store_true",
        help="fail instead of skipping when the Deck is unreachable (use in CI-on-hardware)",
    )


@pytest.fixture(scope="session")
def deck_host(request):
    return request.config.getoption("--deck-host") or sshlib.deck_host()


@pytest.fixture(scope="session")
def deck(request, deck_host):
    """An `Ssh` to the Deck, or a clean skip.

    Unreachable is an ENVIRONMENT condition (exit 3 in the harness taxonomy), never a product
    failure. `--no-skip` turns it into an error, for the unattended runner where a missing Deck IS
    the bug.
    """
    port = sshlib.deck_port()
    if not sshlib.reachable(deck_host, port):
        msg = f"no Deck reachable at {deck_host or '<unset DECK_HOST>'}:{port}"
        if request.config.getoption("--no-skip"):
            pytest.fail(msg)
        pytest.skip(msg)
    return sshlib.Ssh(deck_host, port)


@pytest.fixture(scope="session")
def tunnel(deck, deck_host):
    """`ssh -N -L 9222:localhost:9222`.

    Chrome >= 111 requires `Host: localhost` on the /json endpoints, so we must never talk to the
    Deck's IP directly — the tunnel makes `127.0.0.1` correct by construction (TEST-PLAN §3).

    Shared with `scripts/deckctl.py`, which `just power` and `just soak` drive. One implementation,
    so the two cannot disagree about what "the app is not running" looks like.
    """
    try:
        with sshlib.Tunnel(deck_host, deck.port, CDP_PORT) as tun:
            yield tun.cdp_port
    except (sshlib.NoDevTools, sshlib.DeckUnreachable) as e:
        pytest.skip(str(e))


@pytest.fixture()
def cdp(tunnel):
    """A fresh CDP client per test. Leanback tears targets down; `cdp.py` re-attaches."""
    import cdp as cdplib

    with cdplib.CDP(tunnel) as c:
        yield c


@pytest.fixture()
def leanback(cdp):
    """A CDP client that has confirmed the TV app is loaded.

    Every test starts from a known state (TEST-PLAN §3), never "whatever the last test left behind".
    """
    from lib import probes

    if not cdp.wait_for(probes.ON_LEANBACK_EXPR, timeout=45):
        pytest.skip("engine is not on youtube.com/tv (is the app running and the UA armed?)")
    return cdp


@pytest.hookimpl(hookwrapper=True, tryfirst=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    rep = outcome.get_result()
    if rep.when == "call" and rep.failed:
        _dump_artifacts(item)


def _dump_artifacts(item):
    """Screenshot + logs on failure. Best-effort: a failed dump must not mask the failure."""
    ARTIFACTS.mkdir(exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    base = ARTIFACTS / f"{item.name}-{stamp}"
    try:
        c = item.funcargs.get("cdp") or item.funcargs.get("leanback")
        if c is not None:
            c.screenshot(str(base) + ".png")
    except Exception as e:  # noqa: BLE001 - diagnostics must never raise
        print(f"artifacts: screenshot failed: {e}", file=sys.stderr)
    try:
        d = item.funcargs.get("deck")
        if d is not None:
            _, out, _ = d.run("journalctl --user -n 400 --no-pager 2>/dev/null", check=False)
            (Path(str(base) + ".journal.txt")).write_text(out)
            _, log, _ = d.run(
                "tail -n 400 ~/.local/state/deckback/logs/deckback.log 2>/dev/null", check=False
            )
            if log:
                (Path(str(base) + ".applog.txt")).write_text(log)
    except Exception as e:  # noqa: BLE001
        print(f"artifacts: log capture failed: {e}", file=sys.stderr)
