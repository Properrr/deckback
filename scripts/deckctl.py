#!/usr/bin/env python3
"""Ask the running app on the Deck a question over CDP, and answer with an exit code.

`just power` and `just soak` are shell scripts that measure a Deck. Neither could previously see the
thing it was measuring: `power.sh` sampled the battery without knowing whether a video was playing,
and `soak.sh` checked that a *process* survived resume, which is one of the four clauses of the P6
gate. This is the eye they were missing (T5).

Runs on the WORKSTATION. Opens its own `ssh -L` tunnel, because Chrome >= 111 rejects a `Host:`
header that is not `localhost` on the `/json` endpoints.

Exit codes (.internal/HARNESS.md §1) — the whole point of this file is to tell them apart:

    0  the answer is yes
    2  ASSERT     the product is wrong (playback stalled, position went backwards, software decode)
    3  ENV        the setup is wrong (no video open, video paused, no DevTools, no battery node)
    4  TRANSPORT  the Deck went away

`paused` is ENV and `stalled` is ASSERT, and they are the same `currentTime` reading. Conflating them
sends whoever reads the log to debug the decoder because someone forgot to press play.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_REPO / "scripts"))
sys.path.insert(0, str(_REPO / "tests" / "deck"))

import cdp as cdplib  # noqa: E402
from lib import probes, ssh as sshlib  # noqa: E402

EX_OK, EX_FAIL, EX_ASSERT, EX_ENV, EX_TRANSPORT, EX_USAGE = 0, 1, 2, 3, 4, 5


def _say(msg):
    print(msg, file=sys.stderr)


def _state(cdp):
    return probes.parse_playback_state(cdp.evaluate(probes.PLAYBACK_STATE_EXPR))


def cmd_playing(cdp, args):
    """Is a video actually advancing? Sample twice, `window` seconds apart."""
    before = _state(cdp)
    if not before.get("video"):
        _say("no <video> element on the page. Open a video first — "
             "a power or soak run against a menu measures nothing.")
        return EX_ENV
    if before.get("paused"):
        _say("the video is paused. Press play; this is not a product failure.")
        return EX_ENV

    t0 = time.monotonic()
    time.sleep(args.window)
    elapsed = time.monotonic() - t0
    after = _state(cdp)

    ok, reason = probes.playback_advanced(before, after, elapsed, args.min_fraction)
    _say(f"playback: {reason}")
    if ok:
        return EX_OK
    # Paused/ended/gone between the two samples: the operator's world changed under us. A stalled or
    # rewound video is the product's fault.
    if after.get("paused") or after.get("ended") or not after.get("video"):
        return EX_ENV
    return EX_ASSERT


def cmd_current_time(cdp, _args):
    st = _state(cdp)
    if not st.get("video"):
        _say("no <video> element")
        return EX_ENV
    print(f"{float(st['t']):.3f}")
    return EX_OK


def cmd_decoder(cdp, args):
    """Which decoder is the engine actually using? The P4 gate, reused as a precondition.

    A 9 W budget measured under `Dav1dVideoDecoder` describes a configuration we do not ship. Better
    to refuse the measurement than to publish a number about the wrong software.
    """
    cdp.enable_media()
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline and not cdp.media.decoder_name:
        cdp.pump(1.0)

    name, is_platform = cdp.media.decoder_name, cdp.media.is_platform_decoder
    ok, reason = probes.hardware_decode_verdict(name, is_platform)
    print(f"{name or ''},{'' if is_platform is None else is_platform}")
    _say(f"decoder: {reason}")
    if ok:
        return EX_OK
    if not name:
        # The Media domain never reported. That is a broken probe, not a software decoder — and
        # calling it a software decoder would send the reader to debug VA-API.
        return EX_ENV
    return EX_ASSERT if args.require_vaapi else EX_OK


def cmd_epp(_cdp, args):
    """Read EPP across all cores. Never a verdict: we cannot write it back from the Flatpak."""
    deck = sshlib.Ssh(args.host, args.ssh_port)
    _, out, _ = deck.run(probes.EPP_READ_CMD, check=False)
    try:
        epp = probes.parse_epp(out)
    except probes.NoTelemetry as e:
        _say(f"epp: {e}")
        return EX_ENV
    for cpu in sorted(epp):
        print(f"{cpu},{epp[cpu]}")
    return EX_OK


# `epp` talks to sysfs over plain SSH and needs no browser; everything else needs a CDP tunnel.
_NEEDS_CDP = {"playing": cmd_playing, "current-time": cmd_current_time, "decoder": cmd_decoder}
_NEEDS_SSH_ONLY = {"epp": cmd_epp}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("command", choices=sorted(set(_NEEDS_CDP) | set(_NEEDS_SSH_ONLY)))
    ap.add_argument("--host", default=None, help="DECK_HOST override")
    ap.add_argument("--ssh-port", type=int, default=None)
    ap.add_argument("--cdp-port", type=int, default=9222)
    ap.add_argument("--window", type=float, default=4.0,
                    help="seconds to watch currentTime for (playing)")
    ap.add_argument("--min-fraction", type=float, default=0.5,
                    help="currentTime must advance at least this fraction of wall time")
    ap.add_argument("--timeout", type=float, default=20.0, help="seconds to wait for the Media domain")
    ap.add_argument("--require-vaapi", action="store_true",
                    help="exit 2 when the decoder is not VA-API (decoder)")
    args = ap.parse_args()

    args.host = args.host or sshlib.deck_host()
    args.ssh_port = args.ssh_port or sshlib.deck_port()
    if not args.host:
        _say("no DECK_HOST (set it in .env or pass --host)")
        return EX_ENV
    if not sshlib.reachable(args.host, args.ssh_port):
        _say(f"{args.host}:{args.ssh_port} is unreachable")
        return EX_TRANSPORT

    if args.command in _NEEDS_SSH_ONLY:
        return _NEEDS_SSH_ONLY[args.command](None, args)

    with sshlib.Tunnel(args.host, args.ssh_port, args.cdp_port) as tun:
        with cdplib.CDP(tun.cdp_port) as cdp:
            return _NEEDS_CDP[args.command](cdp, args)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except sshlib.NoDevTools as e:
        _say(f"environment: {e}")
        sys.exit(EX_ENV)
    except sshlib.DeckUnreachable as e:
        _say(f"transport: {e}")
        sys.exit(EX_TRANSPORT)
    except probes.NoTelemetry as e:
        # "The measurement never happened" is never "the measurement passed" (finding F1).
        _say(f"environment: {e}")
        sys.exit(EX_ENV)
    except cdplib.RETRYABLE as e:
        _say(f"transport: {type(e).__name__}: {e}")
        sys.exit(EX_TRANSPORT)
