#!/usr/bin/env python3
"""One cell of the touch bisect: inject the page scripts, dispatch REAL touch, count what arrived.

Driven by scripts/touch-bisect.sh, which supplies the engine and the mode. See that file for what
the experiment can and cannot prove.

The touch is dispatched with CDP `Input.dispatchTouchEvent`, which enters at the browser layer and
produces trusted events — the same kind a panel produces, minus the compositor path. So a page
script that suppresses touch WILL show up here; a compositor-level cause will not.

Exit codes: 0 the cell ran and touch arrived · 2 touch did NOT arrive (a real regression) ·
3 the cell could not be set up (no DevTools, script would not install) — never a silent pass.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_REPO / "scripts"))

import cdp as cdplib  # noqa: E402

EX_OK, EX_ASSERT, EX_ENV = 0, 2, 3


def verdict(stats, taps):
    """Exit code for one cell. Pure, so tests/harness/test_touch_bisect.py can pin it.

    The distinction that matters: a cell that could not be SET UP (no router, no engine, an
    exception) is ENV, never OK. This whole experiment exists to answer "did touch arrive?", and a
    harness that answers "yes" because it never asked is the failure mode the project keeps
    registering.
    """
    if not stats:
        return EX_ENV
    seen = stats.get("sequences")
    if not isinstance(seen, int):
        return EX_ENV
    return EX_OK if seen >= taps else EX_ASSERT

# A page that is not youtube.com/tv: this cell is about the ENGINE and the scripts, and pulling in
# Leanback would add a network dependency and its own listeners to the measurement.
PAGE = (
    "data:text/html,<html><body style='margin:0'>"
    "<div id='t' style='width:100vw;height:100vh'>bisect</div></body></html>"
)

COUNT_EXPR = """JSON.stringify((function(){
  var g = window.__deckbackGestures;
  if (!g) return {router:false};
  return {router:true, stats:g.stats(), queued:g.drain().q.length,
          cursor:(document.documentElement.style.cursor || ''),
          maxTouchPoints:navigator.maxTouchPoints};
})())"""


def dispatch_touch(cdp, x=640, y=400):
    """A press and a release at one point — the minimum that must reach the router as a sequence."""
    cdp.call("Input.dispatchTouchEvent", {
        "type": "touchStart",
        "touchPoints": [{"x": x, "y": y, "id": 1}],
    })
    cdp.call("Input.dispatchTouchEvent", {
        "type": "touchEnd",
        "touchPoints": [],
    })


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--preset", required=True)
    ap.add_argument("--mode", required=True, choices=["router", "router+cursor"])
    ap.add_argument("--taps", type=int, default=3)
    args = ap.parse_args()

    label = f"{args.preset:<8} {args.mode:<14}"
    try:
        cdplib.wait_endpoint(args.port, tries=60)
        with cdplib.CDP(args.port) as cdp:
            cdp.call("Page.enable")
            cdp.call("Page.navigate", {"url": PAGE})
            cdp.wait_for("document.readyState === 'complete'", timeout=20)

            router = (_REPO / "config" / "scripts" / "touch_gestures.js").read_text()
            if not cdp.evaluate(router + "; !!window.__deckbackGestures"):
                print(f"{label}  ENV: the router would not install")
                return EX_ENV
            if args.mode == "router+cursor":
                cdp.evaluate((_REPO / "config" / "scripts" / "hide_cursor.js").read_text())

            cdp.evaluate("window.__deckbackGestures._reset()")
            for _ in range(args.taps):
                dispatch_touch(cdp)

            got = json.loads(cdp.evaluate(COUNT_EXPR) or "{}")
            if not got.get("router"):
                print(f"{label}  ENV: the router vanished mid-cell")
                return EX_ENV
            st = got["stats"]
            code = verdict(st, args.taps)
            ok = code == EX_OK
            print(
                f"{label}  sequences={st['sequences']:<3} emitted={st['emitted']:<3} "
                f"queued={got['queued']:<3} cursor={got['cursor'] or '(unset)':<7} "
                f"maxTouchPoints={got['maxTouchPoints']}  -> "
                f"{'TOUCH ARRIVES' if ok else 'NO TOUCH'}"
            )
            return code
    except Exception as e:  # noqa: BLE001 - a cell that cannot run must not read as a pass
        print(f"{label}  ENV: {type(e).__name__}: {e}")
        return EX_ENV


if __name__ == "__main__":
    sys.exit(main())
