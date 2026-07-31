"""SSH to the Deck. Thin on purpose — the Deck is a dumb target (TEST-PLAN §3).

The exit-code taxonomy from `.internal/HARNESS.md` is the contract this module exists to protect:

    2 = ASSERT     the product is wrong
    3 = ENV        the environment is wrong (no Deck, no udev rule, no binary)
    4 = TRANSPORT  the connection failed

Confusing them is how a broken SSH tunnel gets filed as a product bug, and how `just power` once
reported ``mean 0.00 W … PASS`` on a Deck with no battery telemetry. `SshError` and `DeckUnreachable`
are separate exceptions so a retry decorator can retry the second and never the first.
"""

from __future__ import annotations

import os
import socket
import subprocess
import time


class SshError(RuntimeError):
    """The command ran and failed. This is a *result*, never something to retry."""

    def __init__(self, argv, rc, out, err):
        super().__init__(f"ssh command failed (rc={rc}): {' '.join(argv)}\n{err.strip()}")
        self.rc = rc
        self.out = out
        self.err = err


class DeckUnreachable(ConnectionError):
    """The Deck could not be contacted. Transport: retryable, and never an assertion failure."""


class NoDevTools(RuntimeError):
    """The Deck is reachable but the app is not exposing DevTools. ENVIRONMENT (3), not a defect."""


# The repo root, as lib.sh computes it from its own path before `cd`-ing there. A module-level name
# so the L0 tests can point the derivation at a scratch repo instead of the developer's real one.
_REPO_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))


def _shell_assignments(path):
    """`KEY=value` pairs from a sourceable shell fragment. Not a shell: no expansion, no execution.

    Enough for `.env` and `.steamdeck_auth`, which lib.sh only ever `.`-sources for plain literals.
    A line it cannot parse is skipped rather than guessed at.
    """
    out = {}
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, value = line.partition("=")
                key = key.removeprefix("export ").strip()
                if key.isidentifier():
                    out[key] = value.strip().strip("'\"")
    except OSError:
        pass
    return out


def _deck_files():
    """Merged `.env` then `.steamdeck_auth`, matching lib.sh's source order (the latter wins).

    Read from the repo root *last*, because that is the only directory lib.sh ever reads: it `cd`s
    there first. The cwd is consulted only as a fallback for a caller running from somewhere else.
    """
    merged = {}
    for directory in dict.fromkeys((os.getcwd(), _REPO_ROOT)):
        for name in (".env", ".steamdeck_auth"):
            merged.update(_shell_assignments(os.path.join(directory, name)))
    return merged


def deck_host():
    """The Deck's ssh destination, derived exactly as `scripts/lib.sh` derives it.

    `.steamdeck_auth` (gitignored, `STEAMDECK_USER` + `STEAMDECK_IP`) is the documented place to put
    the Deck's address, and lib.sh composes `DECK_HOST` from it and **exports** it, so every Python
    tool launched through a `scripts/*.sh` recipe inherits a correct value.

    This function used to read only `$DECK_HOST` and a `DECK_HOST=` line in `.env` — neither of which
    a Deck configured the documented way ever sets. That is invisible for as long as every entry
    point is a shell script, and `just touch-probe` is the first recipe to call a Python entry point
    **directly**: it answered "no Deck reachable at <unset DECK_HOST>:22" against a Deck answering
    SSH on the previous line. Same shape as harness.md F14, which hid the fact that the L2 suite had
    never once run, and as the export bug lib.sh's own comment records — "a fix that has to be
    repeated per caller is a fix that will be forgotten by the next caller". So the derivation lives
    here, next to the reader, rather than in the next caller's shim.

    Precedence: `$DECK_HOST` (what lib.sh exports, so a child of a recipe agrees with its parent by
    construction) > `.steamdeck_auth` > `.env`.
    """
    v = os.environ.get("DECK_HOST")
    if v:
        return v
    files = _deck_files()
    user, ip = files.get("STEAMDECK_USER"), files.get("STEAMDECK_IP")
    if user and ip:
        return f"{user}@{ip}"
    return files.get("DECK_HOST") or None


def deck_port():
    """The Deck's ssh port: `$DECK_PORT`, else `STEAMDECK_PORT` from `.steamdeck_auth`, else 22.

    Same failure as `deck_host()` and worse to diagnose: a wrong port is not "no Deck configured",
    it is a connection refused by something that may well be a different machine.
    """
    v = os.environ.get("DECK_PORT") or _deck_files().get("STEAMDECK_PORT")
    try:
        return int(v)
    except (TypeError, ValueError):
        return 22


def ssh_hostname(dest):
    """The hostname inside an ssh destination, or None.

    `DECK_HOST` is an ssh *destination* -- `deck@192.168.1.10`, `deck@[fe80::1%eth0]` -- which is
    what ssh(1) accepts and what a socket does not. `socket.create_connection(("deck@1.2.3.4", 22))`
    raises gaierror, so a reachability probe handed the raw destination answers "unreachable" for
    every Deck that has ever been configured, on a Deck that is answering SSH at that moment.

    That is not hypothetical. Every consumer of `reachable()` -- the `deck` fixture, `deckctl.py`
    (so `just power`, `just soak`), and `cert.py --deck` -- was fed the destination verbatim. With
    `--no-skip` the L2 suite reported exit 2, "the product is wrong", about a healthy product;
    without it, all 23 tests skipped and pytest exited 0. The suite had never once run.
    """
    if not dest:
        return None
    host = dest.rsplit("@", 1)[-1]  # user@host; DECK_HOST never carries a password
    if host.startswith("[") and "]" in host:  # bracketed IPv6 literal, per ssh(1)
        host = host[1 : host.index("]")]
    return host or None


def reachable(host, port=22, timeout=3.0):
    """Is the Deck's SSH port accepting connections?

    Answers about transport only. Whether the *key* is accepted is `require_deck`'s job (lib.sh),
    which every script that reaches the Deck runs first. Deliberately not an ssh handshake: this is
    a session fixture on the hot path, and a false "reachable" here surfaces immediately as an auth
    error from the first real command rather than as a silent skip.
    """
    name = ssh_hostname(host)
    if not name:
        return False
    try:
        with socket.create_connection((name, port), timeout):
            return True
    except OSError:
        return False


class Ssh:
    def __init__(self, host, port=22, timeout=60):
        self.host = host
        self.port = port
        self.timeout = timeout

    def _base(self):
        return [
            "ssh",
            "-p",
            str(self.port),
            "-o",
            "BatchMode=yes",
            "-o",
            "ConnectTimeout=5",
            "-o",
            "StrictHostKeyChecking=accept-new",
            self.host,
        ]

    def run(self, command, check=True, input_bytes=None, timeout=None):
        """Run a shell command on the Deck. Returns (rc, stdout, stderr)."""
        argv = self._base() + [command]
        try:
            p = subprocess.run(
                argv,
                input=input_bytes,
                capture_output=True,
                timeout=timeout or self.timeout,
            )
        except subprocess.TimeoutExpired as e:
            raise DeckUnreachable(f"ssh timed out after {timeout or self.timeout}s: {command}") from e
        out = p.stdout.decode(errors="replace")
        err = p.stderr.decode(errors="replace")
        # 255 is ssh's own failure code (auth, DNS, refused) — the connection, not the command.
        if p.returncode == 255:
            raise DeckUnreachable(f"ssh transport failure to {self.host}: {err.strip()}")
        if check and p.returncode != 0:
            raise SshError(argv, p.returncode, out, err)
        return p.returncode, out, err

    def python(self, source, check=True, timeout=None):
        """Pipe a Python program to the Deck's interpreter over stdin.

        Nothing is ever installed on the Deck, and nothing is left behind: no temp file to clean up
        after a failed run, and no stale copy of a script to be silently re-used by the next one.
        """
        return self.run(
            "python3 -", check=check, input_bytes=source.encode(), timeout=timeout
        )

    def file_text(self, path, check=False):
        rc, out, _ = self.run(f"cat {path} 2>/dev/null", check=check)
        return out if rc == 0 else ""

    def app_running(self, pattern="deckback-launcher"):
        rc, _, _ = self.run(f"pgrep -f {pattern} >/dev/null", check=False)
        return rc == 0


class Tunnel:
    """`ssh -N -L <local>:localhost:<remote>` as a context manager.

    Chrome >= 111 rejects a `Host:` header that is not `localhost` on the `/json` endpoints, so the
    CDP client must never talk to the Deck's LAN IP. Tunnelling makes `127.0.0.1` correct by
    construction rather than by remembering.

    Waits for the forwarded port to accept a connection instead of sleeping a magic number, and
    distinguishes the two ways that wait can end: ssh died (transport) versus ssh is fine but nothing
    is listening on the far side (the app is not running — an environment fact, not a defect).
    """

    def __init__(self, host, port=22, cdp_port=9222, timeout=10.0):
        self.host = host
        self.port = port
        self.cdp_port = cdp_port
        self.timeout = timeout
        self.proc = None

    def __enter__(self):
        argv = [
            "ssh", "-p", str(self.port), "-N",
            "-o", "BatchMode=yes",
            "-o", "ExitOnForwardFailure=yes",
            "-L", f"{self.cdp_port}:localhost:{self.cdp_port}",
            self.host,
        ]
        self.proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                err = self.proc.stderr.read().decode(errors="replace").strip()
                raise DeckUnreachable(f"ssh tunnel to {self.host} died: {err}")
            try:
                with socket.create_connection(("127.0.0.1", self.cdp_port), 0.5):
                    return self
            except OSError:
                time.sleep(0.2)
        self.close()
        raise NoDevTools(
            f"nothing is listening on {self.host}:{self.cdp_port}. Is deckback-launcher running "
            f"with --remote-debugging-port={self.cdp_port}?"
        )

    def __exit__(self, *_):
        self.close()

    def close(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None
