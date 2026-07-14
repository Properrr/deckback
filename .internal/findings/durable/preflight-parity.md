# Preflight parity — local and CI run the SAME gate, or they drift and you ship red

**Status:** ★ ADOPTED 2026-07-13. Applies to every push and every release, on every machine.

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

## What preflight runs (mirrors `.github/workflows/lint.yml` exactly)

| target | checks |
|---|---|
| `shell` | `shellcheck -S warning scripts/*.sh tests/harness/*.sh` + `tests/harness/run.sh` |
| `launcher` | clang-format-18 `--dry-run --Werror` + gcc `Release` build & `ctest` + clang build (`-Wall -Wextra -Werror`) |
| `gn` | `args/*.gn` contain overrides only (no `target_os`/`target_cpu`/`is_debug`/`is_official_build`) |

No Chromium checkout, no Deck, no Docker — it runs on any contributor's machine.

## Local tool requirements (for CI-equivalent results)

- **Pinned lint tools** are auto-bootstrapped: if the system has no `clang-format-18` or `shellcheck`,
  preflight drops the exact versions (`clang-format==18.1.8`, `shellcheck-py`) into a cached venv.
  clang-format **18** is load-bearing — v20 reformats differently and CI (v18) stays red.
- **Launcher toolchain** (must be installed; preflight errors with `EX_ENV` if absent):
  `cmake`, `ninja`, `g++`, `clang++`, and the dev libs `libsystemd-dev libcurl4-openssl-dev
  libpulse-dev libxcb1-dev` — install these so the local build matches CI's fully-featured one.
  Without `libxcb1-dev`/`libpulse-dev`, features compile out and a bug in that code can pass locally
  yet fail CI.
- **Harness suite** needs Python 3 + **Pillow** (`python3-pil`); preflight errors clearly if missing.

## The incident this exists to prevent (2026-07-13)

The seek/ScriptLibrary/build-slimming commits were pushed and CI went red **three times in a row**,
each a different check that the old pre-push hook did not run:

1. **Stale test** — `test_cdp_lib.py` still pointed at `config/av1_steering.js` after the
   ScriptLibrary refactor moved it to `config/scripts/`. The old hook ran shellcheck + clang-format
   but **not the harness suite**, so it never saw the `FileNotFoundError`.
2. **Unformatted C++** — 16 launcher files were not clang-format-18 clean. (The hook *did* cover this,
   but only helps on a machine where `just hooks` was run.)
3. **clang-only `-Werror`** — `TouchModeGuard::poll_ms_` is unused in the no-xcb build of `loop()`
   that the `touchmode_test` target always compiles (the `DECKBACK_HAVE_XCB` define is PRIVATE to the
   `deckback-launcher` target, so test binaries never get it). `gcc` has no `-Wunused-private-field`;
   `clang` does. The old hook ran **neither the gcc nor the clang launcher build**, so it could not
   catch this — and it was *masked* in CI until #2 was fixed, because the clang-format step failed
   first and short-circuited the job.

Root causes: **incomplete local checks**, **two definitions that could drift** (hook vs CI), and
**missing/unpinned local tools**. Preflight closes all three: one definition, shared by hook and CI,
with pinned tools bootstrapped on demand.

## Regression coverage

Each of the three failures above was reproduced against `preflight` and confirmed to exit `2`
(`EX_ASSERT` — "the product is wrong"): the stale path fails `preflight shell`; unformatted code and
a removed `[[maybe_unused]]` each fail `preflight launcher`. If you touch preflight, re-run those
three.
