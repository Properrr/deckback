"""SSH to the Deck, mapped onto the harness exit codes (.internal/HARNESS.md).

2 = ASSERT (product wrong), 3 = ENV (no Deck, no udev rule, no binary), 4 = TRANSPORT. Retry
`DeckUnreachable`, never `SshError`.
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


# The repo root, as lib.sh computes it. Module-level so L0 tests can point it at a scratch repo.
_REPO_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))


def _shell_assignments(path):
    """`KEY=value` pairs from a sourceable shell fragment: no expansion, unparseable lines skipped."""
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
    """Merged `.env` then `.steamdeck_auth` (the latter wins), from the cwd then the repo root."""
    merged = {}
    for directory in dict.fromkeys((os.getcwd(), _REPO_ROOT)):
        for name in (".env", ".steamdeck_auth"):
            merged.update(_shell_assignments(os.path.join(directory, name)))
    return merged


def deck_host():
    """The Deck's ssh destination, derived exactly as `scripts/lib.sh` derives it, or None.

    Precedence: `$DECK_HOST` > `STEAMDECK_USER@STEAMDECK_IP` from `.steamdeck_auth` > `.env`.
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
    """The Deck's ssh port: `$DECK_PORT`, else `STEAMDECK_PORT` from `.steamdeck_auth`, else 22."""
    v = os.environ.get("DECK_PORT") or _deck_files().get("STEAMDECK_PORT")
    try:
        return int(v)
    except (TypeError, ValueError):
        return 22


def ssh_hostname(dest):
    """The hostname inside an ssh destination (`deck@1.2.3.4`, `deck@[fe80::1%eth0]`), or None."""
    if not dest:
        return None
    host = dest.rsplit("@", 1)[-1]  # user@host; DECK_HOST never carries a password
    if host.startswith("[") and "]" in host:  # bracketed IPv6 literal, per ssh(1)
        host = host[1 : host.index("]")]
    return host or None


def reachable(host, port=22, timeout=3.0):
    """Is the SSH port of destination `host` accepting TCP connections? Transport only, no auth."""
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
        """Pipe a Python program to the Deck's interpreter over stdin; nothing is left on the Deck."""
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

    Chrome >= 111 requires `Host: localhost` on `/json`, so CDP must go through the tunnel. Raises
    DeckUnreachable if ssh dies, NoDevTools if nothing listens on the far side.
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
