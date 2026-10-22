---
scope: durable
verified: 2026-07-09
sources:
  - scripts/lib.sh
  - .internal/HARNESS.md
  - .internal/TEST-PLAN.md
---

# Harness — how automation is allowed to report results

Properties of *our* command surface, not of the engine; they carry across Cobalt versions. The
reference doc is `.internal/HARNESS.md` (its §6 is the contract for adding a recipe); this file
records the rules behind it. Most share one shape: **a code path that reports success without
observing the thing it claims.** Look for that shape first when reviewing harness code — it is
invisible in a green run.

## F1. A check that cannot fail is not a check

An empty sysfs read coerced by `awk` to `0` once made `just power` print `mean 0.00 W … PASS` — the
best possible score on the gate, from no data. Any script that adjudicates pass/fail must:

- validate each datum at the point of collection and emit a *missing* marker, never a plausible zero;
- **void the whole run** if any datum is unreadable — a mean over a silently truncated subset
  understates the metric;
- refuse to start when its data source is absent, with an `EX_ENV` exit rather than an empty dataset.

The adjudication rules live in `scripts/lib/power_adjudicate.awk` (testable without hardware) and
are pinned by `tests/harness/test_power_adjudicate.sh`. This is the shell instance of TEST-PLAN §0's
"never assert on a value we chose".

## F2. Fire-and-forget plus `exit 0` is a lie

`steam://rungameid/<id>` returns 0 whether or not anything launched. A launch step must **wait for
observable evidence**: the process exists **and** its DevTools endpoint answers. `remote-run.sh`
polls both; failure is `EX_ASSERT`.

## F3. Exit codes must name whose fault it is

Locked (`scripts/lib.sh`, mirrored in `scripts/lib/exitcodes.py` with an L0 test that they match):
`0` ok · `1` generic · **`2` assertion — the product is wrong** · `3` environment/precondition ·
`4` transport · `5` usage. Only `2` may fail a build. Only `4` may be retried. `deck_or_skip` exits
`0` with a printed reason, so a Deck-less run skips loudly.

## F4. Never mask a failure with the message for a different failure

A skip condition (e.g. `[ -d "$COBALT_TREE/.git" ]` in `scripts/fmt.sh`) is tested *separately*;
real failures propagate. A blanket `|| info "(skipped — no checkout)"` reports every error as the
skip.

## F5. Reach the Deck only through the wrapper

Every SSH goes through `deck_ssh`, which honours `STEAMDECK_PORT` and the optional `sshpass` path.
Raw `ssh "$DECK_HOST"` breaks on a non-default port.

## F6. Package the artifact you just built

`scripts/flatpak.sh` takes the preset as `$1`; `release.sh` passes `release`. A packager hardcoded
to `out/deck` ships whatever stale binary sits there.

## F7 — The L2 harness must be testable without the hardware it tests

With no Deck attached the `tests/deck/` suite skips, and a suite that skips proves nothing. So
**everything in `tests/deck/lib/` that can be checked without hardware is pure and unit-tested at L0
by `tests/harness/test_deck_lib.py`** (CI, every push). Facts it pins:

- **`struct input_event` is 24 bytes on x86-64.** With Python's `struct` `=` prefix, `l` is 4 bytes,
  so `"=llHHi"` silently packs 16-byte events the kernel reads as garbage ("device exists, nothing
  happens").
- **`parse_power_now("")` raises `NoTelemetry`**; `"0"` (valid on mains) returns `0.0`. "No data" vs
  "zero" is the whole gate.
- **ioctl numbers are derived, then checked against the constants**: `ioc()` reimplements the
  kernel's `_IOC`, and the test asserts the hardcoded values agree.
- **`gate` vs `probe` markers.** A `gate` failure is a product defect; a `probe` failure is a finding
  about something TEST-PLAN §2 lists as unverified. Both are reported; neither is `xfail`ed.

**Python mutation testing:** `.pyc` caches key on source mtime in whole seconds plus size, so a
same-size edit within the same second runs the *old* bytecode. `rm -rf __pycache__` (or
`python3 -B`) between rounds.

---

## F8 — A check that cannot fail, part three: the vacuous negative

`just smoke` asserts AV1 reports unsupported through `MediaSource.isTypeSupported`,
`HTMLMediaElement.prototype.canPlayType` and `navigator.mediaCapabilities.decodingInfo`. That alone
proves nothing, so:

- **Baseline:** *before* the steering script is injected, the unsteered engine must report AV1
  **supported** (otherwise an engine with no AV1 decoder passes every steering check).
- **Positive control:** VP9 and H.264 must still report supported (a script that rejects everything
  would pass an AV1-only assertion while YouTube serves nothing).
- **Every predicate must evaluate to exactly `true`.** `Runtime.evaluate` returns `undefined` for a
  misspelt property; `run_assertions()` compares with `is True` and reports the actual value.

### The script must be the one that ships

The steering script lives once, in `config/av1_steering.js`: CMake generates `av1_steering_js.hpp`
from it (failing configure if the file could terminate the raw literal), and `just smoke` injects
the same file via `Page.addScriptToEvaluateOnNewDocument`. An L0 test asserts `navigator.cpp` has no
inline copy. Likewise the P4 hardware-decode verdict is spelled once
(`tests/deck/lib/probes.py:hardware_decode_verdict`); `scripts/cdp.py:MediaState` only accumulates.
A **missing** `kIsPlatformVideoDecoder` is a broken probe, not software decode.

### `scripts/fmt.sh`: a formatter's blast radius must be the diff

- Unknown arguments exit 5 (usage).
- Every step formats only files `git` reports as modified or untracked (never `git ls-files "*.gn"`
  over `cobalt/src`, which leaves the tree dirty and blocks `just sync`).
- `shfmt -i 2` (the repo indents shell with two spaces); clang-format is the pinned one CI uses.

### `in_container bash -lc` + `set -e` + `exit` destroys the exit code

    bash -lc 'set -e;  exit 3'   -> 1     <-- your exit 3 is gone
    bash -lc 'set -e;  exit 0'   -> 1
    bash -lc 'set -e;  true'     -> 0
    bash -c  'set -e;  exit 0'   -> 0     <-- no -l
    bash -lc 'set -u;  exit 3'   -> 3     <-- no set -e

`-l` runs `~/.bash_logout`; Debian's ends with `clear_console`, absent in our image, so the last
command returns 1, and under `set -e` bash replaces your status with it. It bites only when leaving
via the `exit` builtin. Inside `in_container bash -lc`, do not use `set -e`, or `set +e` before
`exit`, or use `bash -c`. Appending `exit 0` is what breaks. `scripts/flatpak.sh` uses `bash -c`.

---

## F9 — Two measurements that were never taken

`just power` must know a video is playing, and `just soak` must check more than "the process still
exists". Both go through `scripts/deckctl.py`, which reads `currentTime` over CDP and answers with an
exit code:

- **`paused` is ENV (3); `stalled` is ASSERT (2)** — same reading, different fault.
- **A silent `Media` domain is a broken probe (3), never software decode (2).**
- **Backwards movement is "the player restarted", not "stalled".** Both fail P6.
- **A draw measured under a software decoder (e.g. `Dav1dVideoDecoder`) is about software we do not
  ship** — `power` refuses the run. It also re-checks playback after the window.

### Some things must be reported and never gated

SteamOS resets EPP on cores 1–7 after resume (steamos#2383); fixing it needs root, infeasible from the
Flatpak. `just soak` counts and names EPP transitions and **passes anyway**. A gate you cannot pass
and cannot fix gets disabled, and then stops reporting too.

---

## F10 — The conformance harness's own reporter hides the failure that matters

`js_mse_eme`'s `window.getTestResults()` (`harness/test.js:619`) buckets tests into `pass`/`fail`; a
test that is `UNKNOWN` (never started) lands in neither, so a wedged suite reports zero failures.
`cert.py` walks `globalRunner.testList` directly and treats `UNKNOWN` as a failure of the *run*.
`TIMEOUT` is a result (the test ran out of time), not `UNKNOWN`.

Guards against silently degrading to green:

- **A rewrite that no-ops.** `MEDIA_PATH`/`CERT_PATH` in `harness/util.js` are repointed at our origin
  so the ~244 media vectors come from a SHA-256-pinned cache; `rewrite_util_js` raises if the
  substitution matches nothing.
- **An insecure origin.** EME needs a potentially-trustworthy origin: `http://localhost` qualifies, a
  LAN IP does not. Asserted before the first test.
- **The phone-home.** The harness POSTs results to `qual-e.appspot.com` when the page URL contains
  `appspot.com`/`googleapis.com`; a `localhost` origin never triggers it. Asserted.
- **A missing baseline.** `cert.py` refuses to run without one, and refuses `--update-baseline`
  under `$CI`.
- **A flaky re-run** is recorded `flaky`, never `pass`. Retry transport, never adjudication.
- **A nondeterministic gate.** `playbackperf-*` (TDP, refresh rate, thermals) is a recorded trend,
  never a build failure.

A vector hash mismatch exits **3**, not 2: "Google changed the bucket" is not "our decoder broke".

## F11 — The first real conformance run: three red tests, zero engine bugs

`just cert conformance-test deck` is **45/46**; the one failure is expected. A red conformance test
is a hypothesis about the engine, and the null hypothesis is that the harness or the server is wrong.

- **`XHRUint8Array`** needs a server route the repo never mentions: it POSTs eight bytes to `/echo`
  and expects them back. A 501 fires the XHR's `load` (not `error`), so the harness reports a
  timeout. `ch.ECHO_ROUTE` / `Handler._serve_echo` implement it, bounded at 64 KiB.
- **`StartPlayWithoutData`** is `PASSED` in the baseline; it failed only as collateral of the
  `/echo` timeout.
- **`MediaElementEvents` is an expected failure against any Chromium-based engine of this era**
  (`Source buffers are updating on duration change.`). `lib/mse/msutil.js:139 setDuration()` assigns
  `ms.duration` and the test (`media/conformanceTest.js:305`) asserts no SourceBuffer is `updating`.
  Blink's `DurationChangeAlgorithm` (`media_source.cc`) takes the deprecated branch while
  `MediaSourceNewAbortAndDuration` is off (it is `experimental`): with `old_duration` = `Infinity`
  before `endOfStream()`, it calls `Remove_Locked` on every SourceBuffer, setting `updating = true`
  synchronously inside the setter. `--enable-blink-features=MediaSourceNewAbortAndDuration` flips
  the check. We do not enable it — it changes `abort()`/`duration` semantics site-wide. Recorded as
  `FAILED` in `tests/cert/expectations/conformance-test.json`.
- **Usage errors are `EX_USAGE` (5).** `argparse` exits 2, which is `EX_ASSERT`; `cert.py`
  overrides `ArgumentParser.error`.
- **A precondition must ask the same question as the thing it guards.** `port_is_free()` binds with
  `SO_REUSEADDR`, matching `Server.allow_reuse_address`; otherwise a `TIME_WAIT` socket from the
  previous run reports a busy port. An L0 test asserts the two agree.

## F12 — "You never built it" is not "the wire broke"

A missing engine binary must be ENV (3), not a transport failure (4) a retry loop will repeat
forever. `smoke.sh` checks `[ -x out/$preset/$target ] || exit 3` (as `cert.sh` does), and
`just smoke` takes a preset (`smoke preset="dev"`). The `bash -lc` + `set -e` trap is in F8.

## F13 — The runner that decides whether to wake you up must not guess

`just deck-ci` (`scripts/deck-ci.py`; decisions in the pure, mutation-tested `scripts/lib/deckci.py`):

- **Retry transport, never adjudication.** `retryable()` is true for exactly one outcome; ENV is not
  retryable either.
- **A run that adjudicated nothing did not pass**: exit **3**, `no adjudicating stage ran — this run
  proves nothing`.
- **A regression outranks the noise that follows it**: a `gate` 2 followed by an SSH drop is a **2**.
- `probe` stages never fail the build and are never swallowed — they land in `summary.json` with a
  pointer to their log.
- **`lib.sh` sets `DECK_HOST` but does not export it**; `deck-ci.sh` exports it (a test pins the
  line), and teardown says so out loud when it cannot run. Teardown goes through `deck_ssh`.

The half that cannot be tested without a Deck contains no decisions; the half with all the decisions
needs no Deck (F7's rule).

## F14 — The L2 suite had never run, because its reachability probe could not parse its own address

`DECK_HOST` is an ssh destination (`deck@192.168.1.10`), not a hostname. `sshlib.reachable()` strips
the `user@` and unbrackets IPv6 via `ssh_hostname()` before opening a socket; without it every Deck
reads unreachable, so the suite skips green (or, under `--no-skip`, fails as exit 2). The same
`reachable()` gates `deckctl.py` and `cert.py --deck`. Its L0 test includes a positive control — a
real listening socket addressed as `deck@127.0.0.1`; tests made only of `assertFalse` pass against a
function hardcoded to `return False`.

## F15 — cert-deck attached to the launcher's own page, which navigated the suite away

The launcher's Navigator re-navigates its page back to `youtube.com/tv` whenever it leaves. So
`cert.py --deck` opens its **own** target (`CDP(port, own_target=True)` → `/json/new`), which the
Navigator does not manage, and closes it on exit. Whoever does not own a target must not assume it
stays put.

## F16 — power.sh required a battery node the only test unit does not have, and would have measured the wrong sign

Each is a hard precondition with L0 coverage in `tests/harness/test_power_probe.sh`:

- **No `power_now` on the OLED.** The gauge exposes only `voltage_now` and `current_now`; watts are
  computed as `V × I` (`scripts/lib/power_watts.awk`).
- **On AC the sign is inverted** — `current_now` is the charging current (a live read gave a
  plausible 3.65 W). `ac_is_online()` refuses the run.
- **A dark panel undercounts.** `panel_is_dark()` refuses unless `DECKBACK_PANEL_OFF_OK=1` accepts a
  non-comparable number.

## Operational notes (on-Deck)

- A screen-blank toggle that `chvt`s away from gamescope's VT deactivates the seat session, and logind
  **revokes** the `uaccess` ACLs on `/dev/uinput` and the gamepad — every input and touch L2 test then
  skips as an environment failure.
- `nohup` over SSH loses a uinput device when logind tears down the session.
- `pkill -f <pattern>` matches its own command line (e.g. the remote `sh -c` an SSH call runs it
  in). Use a pattern that cannot match itself, such as `[d]eckback`.
- When grepping logs for a result, write a unique marker first and grep only after it — otherwise you
  match earlier runs.
