# Preflight parity — local and CI run the SAME gate

**Status:** adopted. Applies to every push and every release, on every machine.

## The rule

**Nothing is pushed that has not passed `scripts/preflight.sh` (a.k.a. `just preflight`), and nothing
is released from a commit that is not green in CI.** The pre-push hook enforces the first; the
release scripts enforce the second. Run `just hooks` once per clone — it is not optional.

`scripts/preflight.sh` is the **single definition** of the pre-push / pre-release gate. Three callers
share it, so none can drift from the others:

- `.githooks/pre-push` → `preflight.sh all`
- `.github/workflows/lint.yml` → `preflight.sh {shell,launcher,gn}` (one per job, preserving CI's
  parallelism)
- `scripts/release-prep.sh` → `preflight.sh all` before it cuts the release branch

`scripts/release.sh` adds the second half: before it burns a Chromium-scale build, it queries GitHub
and refuses unless **every check-run on the tagged commit is `completed/success`** (`FORCE=1` to
override for an offline/emergency build).

## What preflight runs

| target | checks |
|---|---|
| `shell` | `shellcheck -S warning scripts/*.sh scripts/sim/*.sh tests/harness/*.sh` + `tests/harness/run.sh` + `scripts/check-permissions.sh` (no `finish-args` addition against any self-updatable release tag) |
| `launcher` | clang-format `--dry-run --Werror` + gcc `Release` build & `ctest` + clang build (`-Wall -Wextra -Werror`) |
| `gn` | `args/*.gn` contain overrides only (no `target_os`/`target_cpu`/`is_debug`/`is_official_build`) |

No Chromium checkout, no Deck, no Docker — it runs on any contributor's machine. Exit codes follow
the harness taxonomy: `2` a check failed, `3` a required tool/lib is missing, `5` usage.

## Local tool requirements (for CI-equivalent results)

- **Pinned lint tools**: clang-format always comes from a cached venv at exactly
  `CLANG_FORMAT_VERSION` (`scripts/lib.sh`, **23.1.3**, the PyPI wheel), in CI too, because
  clang-format releases format differently; `shellcheck` is bootstrapped there (`shellcheck-py`)
  when the system has none.
- **Launcher toolchain** (must be installed; preflight exits `3` if absent): `cmake`, `ninja`,
  `g++`, `clang++`, and the dev libs `libsystemd-dev libcurl4-openssl-dev libpulse-dev libxcb1-dev`.
  Without `libxcb1-dev`/`libpulse-dev`, features compile out and a bug in that code can pass locally
  yet fail CI.
- **Harness suite** needs Python 3 + **Pillow** (`python3-pil`); preflight exits `3` if missing.

## Why both compilers

clang has warnings gcc lacks (e.g. `-Wunused-private-field`), and test binaries do not get every
private define of the main target (e.g. `DECKBACK_HAVE_XCB`), so a field can be unused only in a test
build. Only running both builds locally catches that.

Origin, in one line: three consecutive red CI pushes — a stale harness path, unformatted C++, and a
clang-only `-Werror` — each slipped through a hook that ran only part of CI. Each was reproduced
against preflight and exits `2`; re-run those three cases if you change preflight.
