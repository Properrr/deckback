# Conformance expectations

One `<suite>.json` per suite: `{"<test name>": "PASSED" | "FAILED" | "OPTIONAL_FAILED" | "TIMEOUT"}`.

`conformance-test.json` was recorded on **2026-07-10** from `out/deck/content_shell` (headless,
Xvfb): **45 PASSED, 1 FAILED**. Every entry in it was read before it was committed, and the single
`FAILED` — `MediaElementEvents` — is explained in finding F11: `js_mse_eme` asserts that no
SourceBuffer is `updating` immediately after `ms.duration` is assigned, and Blink's deprecated
duration-truncation path calls `remove()` on every SourceBuffer from inside that setter. It is an
expected failure of this harness against any Chromium-based engine, not a defect in our build.

The other suites have no baseline yet. `scripts/cert.py` exits **3** in that state rather than
passing a run it has no opinion about.

To record one:

```sh
just build dev
just cert conformance-test dev --update-baseline --freeze-vectors
```

(Note: no `--` before the flags. `just` passes them through, and `cert.py` would reject a stray `--`
as a usage error — exit 5, which is *not* the same as exit 2.)

Then **read the diff before committing it.** A baseline is a claim about what *should* pass, not a
record of what did. Rubber-stamping the first run bakes today's bugs in as tomorrow's expectations.

This is not a slogan. The first real run reported three failures. Two were bugs in *our own*
harness — a missing `/echo` route the suite needs and upstream never ships, and its 10-second
timeout knocking over the test that ran after it. Rubber-stamping would have recorded
`XHRUint8Array: FAILED` as expected, and the day someone fixed `/echo` the gate would have called
the fix a regression. Read F11 before you touch a baseline.

## Rules

- **Only a regression fails the build**: a test that was `PASSED` and now is not.
- A newly-passing test prints `baseline update available` and **never** auto-updates. `cert.py`
  refuses `--update-baseline` when `$CI` is set: a baseline that rewrites itself agrees with whatever
  just happened, which is not agreement.
- A test that never ran (`UNKNOWN`) **fails the run**. A suite that wedged after 3 of 400 tests has
  no opinion about the other 397, and silence is not assent. (The harness's own `getTestResults()`
  drops `UNKNOWN` and `TIMEOUT` entirely, which is why we do not use it.)
- A test that fails and then passes on re-run is recorded **`flaky`**, never `pass`.
- Widevine key systems stay expected-fail until the P7 `AddContentDecryptionModules` patch lands. CI
  must never fetch Google's CDM (`docs/legal.md`), so the automated run is **ClearKey only**.
- Record `harness_commit` and the vector hashes alongside every result. A baseline whose harness you
  cannot name is not a baseline.

Passing `js_mse_eme` is **not** YouTube certification and confers no status.
