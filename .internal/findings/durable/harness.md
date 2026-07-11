---
scope: durable
verified: 2026-07-09
sources:
  - scripts/lib.sh
  - .internal/HARNESS.md
  - .internal/TEST-PLAN.md
---

# Harness — how automation is allowed to report results

Carries across Cobalt versions: these are properties of *our* command surface, not of the engine.
The reference doc is `.internal/HARNESS.md`; this file records the findings behind it.

## F1. A check that cannot fail is not a check (2026-07-09)

`scripts/power.sh` sampled `/sys/class/power_supply/BAT*/power_now` and averaged it in `awk`. When
the glob matched nothing — a docked Deck, a renamed node, an SSH that produced no output — `$uw` was
empty, `awk` coerced `""` to numeric `0`, and the script printed:

```
mean 0.00 W over 300 samples
```

and exited **0**. The P4 gate is *"≤ ~9 W for 1080p VP9"*. **A run that measured nothing therefore
scored the best possible result on the gate it exists to enforce**, and would have "confirmed" the
headline power win of Phase 4.

Reproduced deliberately before fixing (`awk -v u="" 'BEGIN{printf "%.2f", u/1000000}'` → `0.00`).

**Rule.** Any script that adjudicates pass/fail must:
- validate each datum at the point of collection and emit a *missing* marker, never a plausible zero;
- **void the whole run** if any datum is unreadable — a mean over a silently-truncated subset
  understates the metric, which is the same defect wearing a different hat;
- refuse to start when its data source is absent, with an `EX_ENV` exit rather than an empty dataset.

The adjudication rules now live in `scripts/lib/power_adjudicate.awk` (separated so they are testable
without hardware) and are pinned by `tests/harness/test_power_adjudicate.sh`. Restoring the original
one-liner makes 9 of its 10 assertions fail — that is the evidence the test is real.

*Generalization:* this is the shell-script instance of TEST-PLAN §0's "never assert on a value we
chose". `0.00` was not measured; it was manufactured by a type coercion and then asserted against.

## F2. Fire-and-forget plus `exit 0` is a lie (2026-07-09)

`scripts/remote-run.sh` launched the app via `steam://rungameid/<id>`. Steam returns 0 whether or not
it launched anything, so the script printed *"launch request sent to Steam"* and exited 0 with a
stale shortcut id, a dead D-Bus session, or the wrong `DISPLAY` — i.e. with the app not running.

Every future automated on-Deck test begins by trusting that exit code. A launch step must **wait for
observable evidence** that the thing launched: the process exists **and** its DevTools endpoint
answers. Both are now polled, and failure is `EX_ASSERT`.

## F3. Exit codes must name whose fault it is (2026-07-09)

Before this, essentially every failure in every script exited `1` (from `lib.sh:die`). An unattended
runner — or an agent — cannot distinguish "the product regressed" from "no Deck plugged in", so it
either retries real defects or fails builds on transport blips. TEST-PLAN §0 already forbids retrying
an `AssertionError`; that rule is unenforceable without a taxonomy.

Locked (`scripts/lib.sh`): `0` ok · `1` generic · **`2` assertion — the product is wrong** · `3`
environment/precondition · `4` transport · `5` usage. Only `2` may fail a build. `4` is the only code
that may be retried. `deck_or_skip` exits `0` with a printed reason, so a Deck-less run skips loudly.

## F4. Never mask a failure with the message for a different failure (2026-07-09)

`scripts/fmt.sh` wrapped its whole in-container block in `|| info "(tree fmt skipped — no checkout
yet)"`. A `gn format` parse error, a missing `clang-format`, or a container crash all reported that
the checkout was absent — while a checkout demonstrably existed — and then exited 0. The skip
condition (`[ -d "$COBALT_TREE/.git" ]`) is now tested *separately*, and real failures propagate.

## F5. Reach the Deck only through the wrapper (2026-07-09)

`power.sh` and `soak.sh` used raw `ssh "$DECK_HOST"`, bypassing `deck_ssh` and therefore ignoring
`STEAMDECK_PORT` and the optional `sshpass` path that every other script honors. On a Deck with a
non-default SSH port these two recipes — the two *acceptance gates* — were the only ones that broke.

## F6. Package the artifact you just built (2026-07-09)

`scripts/release.sh` built `out/release` (gold + ThinLTO), then called `scripts/flatpak.sh`, which was
hardcoded to package `out/deck`. The tagged release bundle therefore contained whatever stale binary
happened to sit in `out/deck` — or the script `die`d if none existed — while reporting success for a
build it never shipped. `flatpak.sh` now takes the preset as `$1` and `release.sh` passes `release`.

## Standing consequence

`.internal/HARNESS.md` §6 encodes these as the contract for adding a recipe. The three failures above
(F1, F2, F4) share one shape: **a code path that reports success without observing the thing it
claims.** When reviewing harness code, look for that shape first — it is invisible in a green run and
only appears when the check was needed.

## F7 — The L2 harness must be testable without the hardware it tests (2026-07-09)

`tests/deck/` drives the Deck over SSH + CDP + uinput. With no Deck attached its 23 tests all skip,
and **a suite that skips proves nothing** — so every helper it depends on would ship untested, and
the first person to plug in a Deck would be debugging the harness and the product simultaneously,
unable to tell which was lying.

So the rule: **everything in `tests/deck/lib/` that can be checked without hardware is pure, and is
unit-tested at L0 by `tests/harness/test_deck_lib.py`**, which CI runs on every push. That file found
two real bugs before any hardware was involved:

- **`struct input_event` was 16 bytes, not 24.** With Python's `struct` `=` prefix, `l` is *4* bytes
  (standard size), while `struct timeval` on x86-64 is two 8-byte longs. `"=llHHi"` looks obviously
  right and silently packs events the kernel reads as garbage. The only symptom on hardware is "the
  device exists and nothing happens" — which reads as a launcher bug.
- **The power parser would have read an empty sysfs node as `0.0 W`.** The same class of bug as F1
  (`just power` printing `mean 0.00 W … PASS` with no battery telemetry), reintroduced in a new
  language. `parse_power_now("")` now raises `NoTelemetry`, and `"0"` — a valid reading on mains —
  still returns `0.0`. The difference between "no data" and "zero" is the whole gate.

Two more rules the same reasoning produced:

- **ioctl numbers are derived, then checked against the hardcoded constants.** A transposed digit in
  `UI_SET_EVBIT` fails as `EINVAL` on hardware, once, with no clue why. `ioc()` reimplements the
  kernel's `_IOC` macro and the test asserts the constants agree with it.
- **`gate` vs `probe` markers.** A `gate` failure is a product defect. A `probe` failure is a finding
  about something TEST-PLAN §2 already lists as unverified. Both are *reported*; neither is
  `xfail`ed, because an `xfail` hides the answer the probe exists to produce.

### The mutation harness lied to me, and it is worth knowing why

Mutating a Python source file and re-running the tests reported two mutants as SURVIVED that were
in fact caught. `.pyc` caches key on the source's **mtime in whole seconds** plus its size. A
same-size edit inside the same second is invisible to the import system, so the test re-ran against
the *unmutated* bytecode. Any mutation testing of Python here must `rm -rf __pycache__` (or run
`python3 -B`) between rounds. Two of the three "survivors" this exposed were real; one was
`__pycache__`. The others were genuinely untested distinctions, and one was dead code.

---

## F8 — A check that cannot fail, part three: the vacuous negative

*Registered 2026-07-09, while wiring T6's capability probes into `just smoke`.*

`just smoke` now asserts that AV1 reports unsupported through the three APIs YouTube TV probes:
`MediaSource.isTypeSupported`, `HTMLMediaElement.prototype.canPlayType`, and
`navigator.mediaCapabilities.decodingInfo`. That assertion is worth nothing on its own. Two ways it
passes while proving the opposite of what it claims:

- **No AV1 to steer.** If the Cobalt build has no AV1 decoder compiled in, `isTypeSupported(av01…)`
  is already `false` and every steering assertion is green on an engine where the steering script
  never ran. Fixed with a `--baseline`: *before* the script is injected, the unsteered engine must
  report AV1 as **supported**. This is F1's `mean 0.00 W … PASS` in a different costume — a perfect
  score derived from the absence of the thing being measured.
- **Steering that over-reaches.** A script that reported *everything* unsupported satisfies an
  AV1-only assertion perfectly, while YouTube serves nothing at all and the user stares at a black
  screen. Fixed with a mandatory positive control: VP9 and H.264 must still report supported.

A third rule fell out of the same work. **Every predicate must evaluate to exactly `true`.**
`Runtime.evaluate` returns `undefined` for a misspelt property, and `undefined` reads as a clean
failure under a truthiness test and a clean pass under `== true`. Neither is what happened.
`run_assertions()` compares with `is True` and reports the actual value on failure.

### The script must be the one that ships

The steering script lived as a C++ string literal in `navigator.cpp`. A smoke gate holding a *copy*
of it tests the copy. It now lives once, in `config/av1_steering.js`: CMake generates
`av1_steering_js.hpp` from it (failing the configure if the file could terminate the raw literal),
and `just smoke` injects the same file over `Page.addScriptToEvaluateOnNewDocument`. An L0 test
asserts `navigator.cpp` contains no inline copy.

The same rule caught a second duplicate the same day. `MediaState.hardware_decode_ok()` in
`scripts/cdp.py` and `probes.hardware_decode_verdict()` in `tests/deck/lib/probes.py` were two
spellings of the P4 gate, and they had already drifted: the `cdp.py` copy read a **missing**
`kIsPlatformVideoDecoder` as "software decode", which sends whoever reads the log off to debug
VA-API when the real fault is a probe that never reported. `MediaState` now accumulates and does not
adjudicate; the verdict is spelled once.

### `scripts/fmt.sh` formatted the world

`scripts/fmt.sh check` — an argument the script accepted and silently ignored — reformatted all of
`launcher/`, all of `scripts/`, and **148 `.gn`/`.gni` files in `cobalt/src`**, because the `gn
format` step ran over `git ls-files "*.gn"` rather than over what had changed. The Cobalt tree was
left dirty, which is the state `just sync` refuses to run in.

Three fixes, and the general shape is worth keeping: **a formatter's blast radius must be the diff.**

- Unknown arguments now exit 5 (usage). Rejecting what you do not implement beats doing the opposite.
- Every step formats only files `git` reports as modified or untracked.
- `shfmt` gets `-i 2` (the repo indents shell with two spaces; shfmt defaults to tabs) and the host
  `clang-format` is v20 against CI's v18 — never wholesale-format a file you did not touch.

### `in_container bash -lc` + `set -e` + `exit` destroys the exit code

Worth its own heading, because it silently flattens the taxonomy this whole document is about.

    in_container bash -lc 'set -e; exit 0'   -> 1
    in_container bash -lc 'set -e; exit 3'   -> 1     <-- your exit 3 is gone
    in_container bash -lc 'set -e; true'     -> 0
    in_container bash -c  'set -e; exit 0'   -> 0     <-- no -l

The mechanism: `-l` makes it a **login** shell, so bash runs `~/.bash_logout` on the way out. Debian's
default `.bash_logout` ends with `[ -x /usr/bin/clear_console ] && /usr/bin/clear_console -q`, and
`clear_console` does not exist in our image, so the script's last command returns 1. With `set -e`
still in effect during logout, bash replaces your status with that 1. It only bites when the shell
leaves via the `exit` builtin, which is why `set -e; true` looks fine.

So: inside `in_container bash -lc`, either do not use `set -e`, or `set +e` before `exit`, or end on
a plain successful command. Do not "fix" it by appending `exit 0` — that is what breaks. Today only
`fmt.sh` used `set -e` there; `smoke.sh`, `build.sh`, `sync.sh`, `compdb.sh`, `bootstrap.sh`, and
`cert.sh` use `set -u` and are unaffected.

---

## F9 — Two measurements that were never taken

*Registered 2026-07-09, closing the loop on `just power` and `just soak` (T5).*

`just power` sampled the battery for five minutes without knowing whether a video was playing. A
300-second average over a **paused** video passes the ≤9 W gate effortlessly and describes nothing.
That is F1 exactly — a perfect score computed from the absence of the thing being measured — one
layer up from the empty sysfs field, and it survived the fix to F1 because the fix only taught the
script to validate the *reading*, not to check that there was anything to read it about.

`just soak` checked that a *process* still existed after each resume. A launcher that resumes into a
frozen black frame passes that with full marks; the process is the one thing that survives almost
everything. The P6 gate is "alive, position correct, audio back, no dim", and `soak` proved the
clause that was cheapest to prove.

Both now go through `scripts/deckctl.py`, which reads `currentTime` over CDP and answers with an
exit code. The distinctions it exists to keep straight:

- **`paused` is ENV (3); `stalled` is ASSERT (2)** — and they are the same `currentTime` reading.
  Getting them backwards sends whoever reads the log to debug VA-API because someone forgot to press
  play, or files a real stall as operator error and closes the ticket.
- **A silent `Media` domain is a broken probe (3), never software decode (2).** Absence of a decoder
  name is absence of evidence.
- **Backwards movement is "the player restarted", not "stalled".** Both fail the P6 gate; only one
  sends you hunting a hang that never happened.
- **A draw measured under `Dav1dVideoDecoder` is a number about software we do not ship.** `power`
  refuses the run rather than publishing a mysteriously bad score about the wrong configuration.

### Some things must be reported and never gated

SteamOS resets EPP on cores 1–7 after resume (steamos#2383). Writing it back needs root, so it is
infeasible from the Flatpak: we can measure it and we cannot fix it. `just soak` therefore counts the
cycles on which EPP moved, names the transition, and **passes anyway**. A gate you cannot pass and
cannot fix is a gate someone disables — and a disabled gate stops reporting too. This is the same
`gate` vs `probe` split the L2 suite uses, reaching a shell script.

---

## F10 — The conformance harness's own reporter hides the failure that matters

*Registered 2026-07-09, while building `just cert` (T7).*

`js_mse_eme` exposes `window.getTestResults()`, and TEST-PLAN §6 originally said to scrape it. Read
what it does (`harness/test.js:619`): it walks the test list and appends each test to `pass` or
`fail` — and a test whose outcome is `UNKNOWN` (never started) or `TIMEOUT` matches neither branch,
so it lands in neither bucket. A suite that wedges on test 3 of 400 returns `{pass: {…3 tests…},
fail: {}}`. Zero failures. Green.

That is the same shape as `mean 0.00 W … PASS` and as a `just test-deck` run where all 23 tests
skip: **a verdict computed from tests that did not happen.** So `cert.py` walks
`globalRunner.testList` directly and treats `UNKNOWN` as a failure of the *run*, distinct from a
regression. A suite that never reached 397 of its tests has no opinion about them, and silence is
not assent.

`TIMEOUT` is not `UNKNOWN`. The harness reached that test and the test ran out of time — a result.
`UNKNOWN` means it never started. Conflating them makes the no-progress watchdog either blind (if
`UNKNOWN` counts as finished) or hysterical (if `TIMEOUT` does not).

### Everything that can silently degrade to "green" got a guard

- **A rewrite that no-ops.** We repoint `MEDIA_PATH`/`CERT_PATH` in `harness/util.js` at our own
  origin so the ~244 media vectors are served from a SHA-256-pinned cache. If a harness bump renames
  those constants, a `re.sub` that quietly matches nothing sends every vector straight back to
  Google's bucket, unhashed — and the run still passes. `rewrite_util_js` raises instead.
- **An insecure origin.** EME is gated on a potentially-trustworthy origin. `http://localhost`
  qualifies; `http://192.168.x.y:8000` does not. Serve over a LAN IP and *every* EME test fails, for
  a reason that has nothing to do with our engine. Asserted before the first test.
- **The phone-home.** The harness POSTs results to `qual-e.appspot.com` when the page URL contains
  `appspot.com`/`googleapis.com`. A `localhost` origin never triggers it, so the same tunnel that
  buys the secure context also buys the silence. Asserted, not assumed.
- **A missing baseline.** Some tests are legitimately `OPTIONAL_FAILED`; some fail on known gaps of
  ours. With no baseline, `adjudicate()` would classify every test as "new" and pass. `cert.py`
  refuses to run without one, and refuses `--update-baseline` under `$CI` — a baseline that rewrites
  itself agrees with whatever just happened, which is not agreement.
- **A flaky re-run.** A test that fails and then passes on re-run is recorded `flaky`, never `pass`.
  §0's rule reaches L3: retry transport, never adjudication.
- **A nondeterministic gate.** `playbackperf-*` depends on TDP, refresh rate, and thermals. It is
  recorded as a trend and never fails a build, because a gate that fails randomly is a gate someone
  disables — and then it stops reporting too. (Same reasoning as EPP in F9.)

A hash mismatch on a vector exits **3**, not 2. "Google changed the bucket" and "our decoder broke"
are different sentences, and at 3 a.m. only one of them is worth waking up for.

## F11 — The first real conformance run: three red tests, zero engine bugs

*Registered 2026-07-10, from the first `just cert conformance-test deck` runs against a real
`content_shell` (`out/deck`, built 2026-07-09).*

F10 built the gate. This is what happened when it first met an engine. The suite reported
**43/46, three FAILED**: `XHRUint8Array`, `MediaElementEvents`, `StartPlayWithoutData`. Every one of
them looked like a media bug. None of them was.

### `XHRUint8Array` — a server route we never implemented

The test POSTs eight bytes to `/echo` and asserts they come back unchanged. `/echo` appears exactly
once in the entire `js_mse_eme` tree — in the call site. Google's runner answered it server-side and
the repository does not mention that a route is expected.

Our server had no `do_POST`, so `SimpleHTTPRequestHandler` answered 501. **A 501 fires the XHR's
`load` event, not `error`** — `getResponseData()` then trips the harness's internal
`assert(status is 2xx)`, `runner.succeed()` is never reached, and ten seconds later the harness
prints `Test 7:XHRUint8Array TIMED OUT!`. A missing route in *our* code, reported as a timeout, in a
suite whose entire purpose is to catch a slow decoder.

Fixed: `ch.ECHO_ROUTE`, `Handler._serve_echo`, bounded at 64 KiB — an unbounded read on the only
thread that can tell you the suite hung is its own trap.

### `StartPlayWithoutData` — collateral damage

It has never failed since `/echo` was fixed (four consecutive runs). The one failure was in the same
run as the 10-second `XHRUint8Array` timeout that preceded it. Recorded as `PASSED` in the baseline:
if it flakes again the gate goes red, which is the correct outcome, because we would want to know.

### `MediaElementEvents` — the harness contradicts the MSE spec, and Blink obeys the spec

This one is real, permanent, and ours to *document*, not to fix. The failure message is
`Source buffers are updating on duration change.`

`lib/mse/msutil.js:139 setDuration()` trims every SourceBuffer to the target duration with
`remove()`, waits for each `update`, then assigns `ms.duration` and immediately calls back. The test
(`media/conformanceTest.js:305`) asserts that no SourceBuffer is `updating` at that moment.

Instrumented against our engine, that assertion is false — **and it is false for `videoSb`, which had
nothing to remove** (its buffered range ends at 0.999 s; the target duration is 1.0 s). That detail
is what kills the obvious "a straddling AAC frame survived the `remove()`" theory. The cause is in
`third_party/blink/renderer/modules/mediasource/media_source.cc`, `DurationChangeAlgorithm`:

```cpp
if (!RuntimeEnabledFeatures::MediaSourceNewAbortAndDurationEnabled() &&
    new_duration < old_duration) {
  // Deprecated behavior: ... call remove(new duration, old duration) on all
  // objects in sourceBuffers.
  for (unsigned i = 0; i < source_buffers_->length(); ++i)
    source_buffers_->item(i)->Remove_Locked(new_duration, old_duration, ...);
}
```

`MediaSourceNewAbortAndDuration` is `status: "experimental"` in `runtime_enabled_features.json5`, so
it is **off** in every shipping build. `old_duration` is `Infinity` before `endOfStream()`, so
`1.0 < Infinity` holds and Blink calls `Remove_Locked` on *every* SourceBuffer — setting `updating =
true` on both, synchronously, inside the `duration` setter. One task later both are false again and
playback proceeds normally.

Confirmed by falsification, not by reading: re-running the same instrumented page with
`--enable-blink-features=MediaSourceNewAbortAndDuration` flips the callback's check from
`updating=true,true` to `false,false`. The deprecated branch is the whole mechanism.

So the harness encodes an assumption about Cobalt's own pre-Chrobalt MSE implementation that Blink's
does not satisfy. **`MediaElementEvents` is an expected failure of `js_mse_eme` against any
Chromium-based engine of this era**, and it is in `tests/cert/expectations/conformance-test.json` as
`FAILED` with this paragraph as its justification. We do not enable the experimental feature to make
a test green: it changes `abort()` and `duration` semantics site-wide, and Leanback is what has to
keep working, not the scoreboard.

### The lesson, which is the same lesson every time

A red conformance test is a *hypothesis about the engine*, and the null hypothesis is that the
harness or the server is wrong. Two of three reds here were our own plumbing; the third was a
20-year-old spec deprecation. Had we rubber-stamped the first run — which
`tests/cert/expectations/README.md` explicitly forbids — the baseline would today record
`XHRUint8Array: FAILED`, and the day someone fixed `/echo` for an unrelated reason, the gate would
have called it a regression.

### Two harness bugs the run also exposed

- **`argparse` exits 2 on a usage error, and 2 is `EX_ASSERT`.** `just cert conformance-test deck --
  --freeze-vectors` (one `--` too many) reported *"a test that used to pass does not"* to its caller.
  A mistyped flag must never be indistinguishable from a conformance regression. `cert.py` now
  overrides `ArgumentParser.error` to exit `EX_USAGE` (5).
- **A precondition check stricter than the precondition.** `port_is_free()` bound without
  `SO_REUSEADDR`; `Server.allow_reuse_address` is `True`. A `TIME_WAIT` socket from the previous run
  therefore made the guard report `port 8000 is busy` — exit 3 — about a port the server was about to
  bind without complaint. Two `just cert` runs back to back, second one dead on an environment error
  about nothing. The guard now asks the question the server will ask, and an L0 test asserts the two
  agree.

## F12 — "You never built it" is not "the wire broke"

*Registered 2026-07-10, while building the unattended runner (`just deck-ci`).*

`just smoke` exited **4 (TRANSPORT)** on this machine. Transport means *retry me*. It would have been
retried forever, because the real problem was that `out/dev/content_shell` does not exist — only
`out/deck` does.

Two defects, one symptom:

1. **`smoke.sh` never checked that the engine exists.** It launched nothing, and `cdp.py` then waited
   twenty seconds for a DevTools endpoint that nobody was serving and correctly reported a transport
   failure. `cert.sh` has always had `[ -x out/$preset/$target ] || exit 3`. `smoke.sh` did not.
2. **`just smoke` took no preset**, so it could only ever look at `out/dev`. The recipe was
   `smoke:`, not `smoke preset="dev":`.

The unattended runner is what made this urgent. A human reads "DevTools endpoint never came up",
thinks for two seconds, and types `just build dev`. A retry loop reads exit 4 and tries again.

Both fixed. `just smoke deck` then passed against production `youtube.com/tv`: `ytlr-app` present, the
TV UA sticky across navigation, all three AV1 steering APIs asserted, VP9 and H.264 positive controls
intact, and — the check that makes the rest mean anything — the **baseline** confirming the unsteered
engine reports AV1 as *supported*, so the steering assertions are not vacuous.

### A sharpening of F8

While here, the login-shell trap was measured precisely rather than described:

```
dev image: bash -lc 'set -u;  exit 3'  -> 3
dev image: bash -c  'set -u;  exit 3'  -> 3
dev image: bash -lc 'set -eu; exit 3'  -> 1     <-- .bash_logout under `set -e`
```

So `-l` alone is harmless; it is `-l` **plus `set -e`** that lets Debian's `~/.bash_logout` (whose
last command needs `/usr/bin/clear_console`, absent in our image) overwrite a deliberate exit status.
`cert.sh` and `smoke.sh` use `set -u` and are safe. `scripts/flatpak.sh` used `bash -lc` with
`set -euo pipefail` and survived only because the trixie pack image happens to have `clear_console`;
it now uses `bash -c`, which is the kind of luck not worth depending on.

## F13 — The runner that decides whether to wake you up must not guess

*Registered 2026-07-10, with `just deck-ci` (`scripts/deck-ci.py`, `scripts/lib/deckci.py`).*

An unattended runner turns a pile of exit codes into one number and, on the strength of it, either
stays quiet or pages someone. Three rules, all of them things this repo already learned separately,
now encoded in one pure module with mutation-tested coverage:

  * **Retry transport, never adjudication.** `retryable()` returns true for exactly one outcome.
    ENV is deliberately not retryable either: a missing Steam shortcut will still be missing in ten
    seconds, and retrying it only delays the report.
  * **A run that adjudicated nothing did not pass.** Deploy green, launch green, no test run: exit
    **3**, reason `no adjudicating stage ran — this run proves nothing`. This is F1 generalised. It is
    the one bug in the runner that would make CI green for no reason, and it has the loudest test.
  * **A regression outranks the noise that follows it.** `gate` exits 2, then SSH drops during
    `cert`: the run is a **2**, not a 4. Answering 4 invites a retry, and the retry buries the defect.

`probe` stages are non-adjudicating: per `tests/deck/conftest.py`, a probe failure is *a finding to
register, not a regression*. It never fails the build and it is never silently swallowed — it lands
in `summary.json` and gets a paragraph in the console summary pointing at its log. Same posture, and
same reason, as `playbackperf-*` in `cert.py`: a gate that fires randomly is a gate someone turns
off, and then it stops reporting too.

Three bugs the work surfaced, all of the same family:

  * **`argparse` exits 2, and 2 is `EX_ASSERT`.** A mistyped flag on the runner that decides whether
    to page someone reported "the product is wrong". `cert.py` had it too (F11). The taxonomy and the
    fix now live once, in `scripts/lib/exitcodes.py`, and an L0 test asserts they match `lib.sh`.
  * **`lib.sh` sets `DECK_HOST`; it does not export it.** `deck-ci.py`'s teardown read it from an
    environment that never had it, returned immediately, and left the app running on the Deck — where
    the next `just power` would sample a video the *previous* run started. A cleanup step that
    silently no-ops is worse than none, because you stop looking for the mess. `deck-ci.sh` exports
    it, teardown says so out loud when it cannot run, and a test pins the export line.
  * **Teardown called `ssh` directly**, ignoring `DECK_PORT` and the optional `sshpass` that
    `deck_ssh` exists to handle. It now goes through `deck_ssh`.

The runner is split so that the half which cannot be tested without a Deck contains no decisions, and
the half which contains all the decisions needs no Deck. That is F7's rule applied to the thing that
drives F7's harness.


## F14 — The L2 suite had never run, because its reachability probe could not parse its own address

*Registered 2026-07-10, the first time `just test-deck` was pointed at a live Deck.*

Every `tests/deck/` test depends on the session `deck` fixture, which calls
`sshlib.reachable(deck_host, port)` and skips (or, under `--no-skip`, **fails**) if it returns false.
`reachable()` did `socket.create_connection((host, port))` — but `host` is `DECK_HOST`, which
`lib.sh` builds as `${STEAMDECK_USER}@${STEAMDECK_IP}`, i.e. `deck@192.168.1.10`. That is an ssh
*destination*, not a hostname; a socket cannot parse the `user@` prefix and raises `gaierror`. So
`reachable()` returned **false for every Deck that has ever been configured**, on a Deck answering
SSH at that moment.

The two ways this hid:

  * default (`skip`): all 23 tests skipped, pytest exited **0**, and the suite reported a clean pass
    having tested nothing — F1 again, in the runner that is supposed to enforce F1.
  * `--no-skip` (the mode `deck-ci` uses): the skip became a **fail**, so the suite reported exit
    **2**, "the product is wrong", about a healthy product on the far side of a working SSH link.

What makes this an F-series finding and not a one-line bug: the L0 test that "covered" `reachable()`
had three assertions, all `assertFalse` (None, empty string, a closed port). **A function hardcoded
to `return False` passes every one of them.** The check could not fail, so it never caught that the
function could not succeed. The fix (`ssh_hostname()` strips the `user@` and unbrackets IPv6) ships
with the positive control the old suite lacked: a real listening socket, addressed through a
`deck@127.0.0.1` destination — the one assertion that goes red against the old code.

The same `reachable()` is called by `deckctl.py` and `cert.py --deck`, so `just power`, `just soak`,
and `just cert-deck` were blocked by it too. `just soak` "has never run" (§2) in part because it
*could* not.


## F15 — cert-deck attached to the launcher's own page, which navigated the suite away

*Registered 2026-07-10, first real `just cert-deck` run.*

`cert.py --deck` served the `js_mse_eme` harness and drove it over CDP by attaching to the first
`page` target — which on the Deck is the one `deckback-launcher` owns. The launcher runs a Navigator
(`navigator.cpp`) that watches its page and re-navigates it back to `youtube.com/tv` the instant it
leaves. So cert loaded `http://localhost:8000/main.html`, the launcher yanked it home mid-load, and
the run died with `the harness never installed window.globalRunner (page failed to load)` — an ENV
error that looks exactly like a broken tunnel or a wrong origin, none of which it was. The tunnel was
fine; the app under test was fighting the test for control of the page.

The fix gives cert its *own* page target (`CDP(port, own_target=True)` → `/json/new`), which the
launcher's Navigator does not manage, and closes it on exit. The launcher's navigation policy is not
what a media-conformance suite is testing, and it must not be in the loop. This is the mirror image
of the lesson the launcher already embodies for its own UI (re-inject on every navigation because
Leanback tears targets down): whoever does not own the target does not get to assume it stays put.


## F16 — power.sh required a battery node the only test unit does not have, and would have measured the wrong sign

*Registered 2026-07-10, first attempt to run `just power` on the OLED.*

Three ways the P4 battery gate produced, or would have produced, a confident number about nothing —
each one F1's shape (a measurement of the absence of the thing being measured), each now a hard
precondition with L0 coverage in `tests/harness/test_power_probe.sh`:

  * **No `power_now` on the OLED.** `power.sh` globbed `/sys/class/power_supply/BAT*/power_now`; the
    Galileo gauge is charge-based and exposes only `voltage_now` and `current_now` — `find /sys -name
    power_now` is empty. The gate exited 3 forever, so P4 could not run at all on the one unit this
    project owns, and "environment" reads like someone else's problem so nobody chased it. Watts are
    now computed as `V × I` when `power_now` is absent (`scripts/lib/power_watts.awk`, where the
    multiply lives so a test can execute the line that decides what the gate measures).
  * **On AC, the sign is inverted.** `current_now` on the charger is the *charging* current, so
    `V × I` measures energy going *into* the battery. Measured live: **3.65 W** — plausible, under the
    9 W budget, and the opposite of app draw. `ac_is_online()` now refuses the run.
  * **A dark panel undercounts by watts.** An OLED with the screen blanked (a burn-in habit; people
    run a blank-toggle over SSH) draws far less than a real session, on a device whose whole gate is
    "≤ ~9 W". `panel_is_dark()` refuses unless `DECKBACK_PANEL_OFF_OK=1` explicitly accepts a
    non-comparable number.

The blank-toggle also `chvt`s away from gamescope's VT, which deactivates the seat session and makes
logind **revoke** the `uaccess` ACLs on `/dev/uinput` and the gamepad — so an unrelated
convenience script silently turns every input and touch L2 test into an environment skip. Recorded
here because the next person to see "gamepad not found" on a working Deck will not connect it to a
screen-blanking script three terminals away.
