# Deckback — Harness

The single entry point for **building, packaging, deploying, running and testing** Deckback.
Written to be usable two ways, and the constraints of both are stated where they differ:

- **By a human** at a terminal, who wants to know what a command does before running it.
- **By an agent/LLM or an unattended runner**, which cannot read prose output and must decide
  from an **exit code** alone whether the product is broken, the environment is broken, or the
  network is broken.

Companions: `.internal/TASKS.md` (what to build) · `.internal/TEST-PLAN.md` (what is actually proven)
· `.internal/steamdeck-cobalt-youtube-plan.md` (why) · `.internal/MIGRATION.md` (the Cobalt bump).

Status date: 2026-07-10. Pin: `DEPS.pin = a9181df8` (m114).

---

## 0. The one rule

**Every recipe delegates to `scripts/*.sh`.** `just` is a menu, never logic. CI and the unattended
runner call the scripts directly, so anything expressed only in the `justfile` does not exist as far
as automation is concerned.

Every script sources `scripts/lib.sh`, which sets `set -euo pipefail`, `cd`s to the repo root, loads
`.env` + `.steamdeck_auth`, and defines the exit-code taxonomy and the Deck-access helpers. Two
exceptions run *on the Deck* and deliberately do not source it: `scripts/audio-repair.sh` and
`scripts/install-audio-repair.sh`.

---

## 1. Exit-code taxonomy — read this before automating anything

`"the build failed"` is useless at 3 a.m. and useless to an agent. Exit codes distinguish **whose
fault it is**. Defined in `scripts/lib.sh`; TEST-PLAN §6.5 makes this mandatory for the cert harness,
and it is cheap enough to apply everywhere.

| Code | Name | Means | An automated runner should |
|---|---|---|---|
| `0` | ok | success | continue |
| `1` | `EX_FAIL` | generic failure; a tool we shelled out to crashed | investigate; treat as infra until proven otherwise |
| `2` | `EX_ASSERT` | **the product is wrong** — an assertion or gate failed | **fail the build. This is the only code that means a regression.** |
| `3` | `EX_ENV` | precondition missing: no checkout, no Docker, no battery node, no Steam shortcut | fix the environment; never a regression |
| `4` | `EX_TRANSPORT` | Deck unreachable, SSH dropped, CDP endpoint absent | **retry** (per TEST-PLAN §0: retry transport, never adjudication) |
| `5` | `EX_USAGE` | the script was invoked wrong | fix the caller |

Helpers: `die_assert`, `die_env`, `die_transport`, `die_usage`, and bare `die` (= 1).
`deck_or_skip` exits **0** with a printed reason when no Deck is attached, so a Deck-less CI run does
not fail — but it can never skip silently.

> **The failure mode this taxonomy exists to prevent.** `just power` once printed
> `mean 0.00 W over 300 samples` and exited **0** when the battery sysfs node was missing, because
> `awk` coerces `""` to `0`. The P4 gate is *"≤ 9 W"* — so a run that measured **nothing** scored a
> perfect pass. Regression-tested now in `tests/harness/test_power_adjudicate.sh`.
> **A check that cannot fail is not a check.**

---

## 2. Execution context — where a command actually runs

This is the harness's least obvious property. Three different machines are involved.

| Context | What runs there | Recipes |
|---|---|---|
| **Workstation (host)** | `just` itself, the launcher build (CMake), all SSH orchestration | everything below unless noted |
| **Container** (Docker/Podman, Debian 12) | anything touching the Chromium/Cobalt tree — `gclient`, `gn`, `autoninja`, `flatpak-builder` | `bootstrap` `sync` `gen` `build` `compdb` `smoke` `flatpak` `release` |
| **The Deck** (over SSH) | the app, and the two Deck-local installers | `deploy` `run` `remote-run` `logs` `debug` `power` `soak` |
| **The Deck (typed there by hand)** | `install.sh`, `install-audio-repair.sh` — they need a local `.flatpak` and a local systemd user session | `install` `audio-repair` |

**Builds never run on the Deck.** The Deck is a deploy/test target only.

---

## 3. Recipe reference

`H` = needs a human · `D` = can destroy state · `M` = machine-readable output.
"Deck" means an SSH-reachable Steam Deck; "tree" means the Cobalt checkout; "ctr" means Docker.

| Recipe | Does | Needs | H | D | M | Non-zero exits |
|---|---|---|---|---|---|---|
| `just bootstrap` | build dev image; `gclient config` + first sync at `DEPS.pin` | ctr, ~50 GB disk | | | | 1,3 |
| `just sync` | `gclient sync -r $(cat DEPS.pin)`, re-apply `patches/` | tree, ctr | | | | 1,3 |
| `just gen <preset>` | `gn gen out/<preset>` from `args/common.gn` + `args/<preset>.gn` | tree, ctr | | | | 1,3,5 |
| `just build [preset]` | `autoninja content_shell` + the launcher | tree, ctr | | | | 1,3 |
| `just launcher [build\|test]` | build/test **only** the launcher | *nothing* | | | ~ | 1,5 |
| `just test-harness` | L0 tests of the harness's own shell/awk/python logic | *nothing* | | | ~ | 1 |
| `just test-deck [args]` | **L2**: pytest drives the Deck over SSH + a CDP tunnel (+uinput) | Deck **running the app**, `pytest` on the workstation | | ~ (uinput taps/presses) | ✓ `artifacts/` on failure | 2,3,4 |
| `just compdb` | `compile_commands.json` for clangd | tree, ctr, `out/dev` | | | ✓ | 1 |
| `just fmt` | clang-format launcher + tree; `gn format`; `shfmt` | optional ctr | | ✓ writes files | | 1 |
| `just smoke` | Xvfb boot + CDP assertion + screenshot — **the CI gate** | tree, ctr | | | ~ | 2 (assertion), 1 |
| `just deploy [host]` | rsync a stripped `out/deck` bundle to `~/cobalt-yt/` | `out/deck`, Deck | | | | 1,3,4 |
| `just run` | SSH-launch the deployed binary, tee to a remote log | deployed Deck | ✓ foreground | | | 3,4 |
| `just remote-run` | launch the **installed Flatpak** through Steam in Game Mode, then **verify** it came up | installed Deck | | | | 2,3,4 |
| `just logs` | tail the remote app log + journal | Deck | ✓ read by eye | | | — |
| `just debug` | `ssh -L 9222` tunnel for `chrome://inspect` | Deck | ✓ browser | | | 3,4 |
| `just power [secs]` | assert playback + VA-API, sample draw → CSV, adjudicate vs the ≤9 W gate | Deck **on battery**, video playing | ✓ open a video | | ✓ CSV | 2,3,4,5 |
| `just soak [n]` | *n* × suspend/resume; app alive **and** `currentTime` advanced; reports EPP | Deck, video playing | ✓ open a video | ✓ suspends HW | | 2,3,4,5 |
| `just cert [suite] [preset]` | self-hosted `js_mse_eme` headless; fail only on a **regression** | `out/<preset>`, ctr, network | | | ✓ JUnit+JSON | 2,3,4 |
| `just cert-deck [suite]` | same suites against a Deck already running the app | Deck, app up | | | ✓ JUnit+JSON | 2,3,4 |
| `just deck-ci [--full] [--dry-run]` | **unattended**: deploy → launch → gate → cert → teardown; aggregates exit codes | **Deck**, ctr | ✗ (`--dry-run` is safe) | ✓ kills the app on the Deck | ✓ TRANSPORT only | 0,1,2,3,4,5 |
| `just flatpak-lint` | manifest + metainfo + desktop gates | pack image only | | | | 2,3,5 |
| `just flatpak [preset]` | lint, stage `out/<preset>` → `.flatpak` bundle; **asserts the packaged sandbox grants dri+input** | `out/<preset>`, ctr | | ✓ wipes `build-dir` | | 1,2,3 |
| `just install` | install bundle, **verify** evdev was granted, add to Steam | **run on the Deck** | ✓ prints steps | | | 1,2,3 |
| `just audio-repair` | install the host-side mute-recovery user service | **run on the Deck** | | | | 1 |
| `just release <tag>` | gold+ThinLTO build → bundle → checksums → draft GH release | tree, ctr, `gh` | ✓ publishes draft | | ✓ SHA256SUMS | 1,5 |
| `just patch-new <name>` | export `cobalt/` HEAD into `patches/` | tree | | | | 1,5 |
| `just patch-refresh` | re-export **all** patches from `cobalt/` history | tree | | ✓ `rm patches/*.patch` | | 1,5 |

**`just smoke` doubles as the R1/R2 canary.** Run nightly against production `youtube.com/tv`: a
server-side Leanback change that breaks the TV UA should page us the night it happens. It gates on
three things, and each was chosen because its failure is otherwise *silent*:

1. the TV UA lands on Leanback (`ytlr-app`, no `app=desktop`, no cast screen);
2. `--assert-ua` — the UA override **survived** that navigation. When the sticky override is lost,
   the page keeps rendering and every later probe measures the desktop site instead;
3. `--assert-av1-steering` — AV1 reports unsupported through all three APIs YouTube probes
   (`MediaSource.isTypeSupported`, `canPlayType`, `mediaCapabilities.decodingInfo`) **and VP9/H.264
   still report supported**. Without that second half, steering that broke in the direction of
   "everything unsupported" would pass an AV1-only check while YouTube served nothing.

Smoke injects `config/av1_steering.js` — the very file CMake compiles into the launcher — so it
tests the script that ships, not a copy of it. Before injecting, it asserts the *unsteered* engine
says AV1 **is** supported (`--baseline`). On a Cobalt build with no AV1 decoder at all, every
steering assertion would otherwise pass while proving nothing: `just power` scoring `mean 0.00 W …
PASS` from a battery with no telemetry, in a different costume.

---

## 4. The two chains

### A. Clean checkout → `.flatpak` bundle

```
just bootstrap        # once: dev image + gclient sync at DEPS.pin  (~45 GB, ~hours)
just gen deck         # gn gen out/deck  (args/common.gn + args/deck.gn layered on gn.py's config)
just build deck       # autoninja content_shell + the launcher
just flatpak deck     # stage out/deck -> io.github.properrr.deckback.flatpak
```

`just release <tag>` is the tagged variant: it builds `out/release` (gold + ThinLTO) and calls
`flatpak.sh release`. **Passing the preset is load-bearing** — `flatpak.sh` defaults to `deck`, and
until this was fixed `just release` silently bundled whatever stale binary sat in `out/deck` while
reporting success.

### B. Bundle → running on the Deck

Three paths that share no state. Pick deliberately.

| Path | Command | Runs | Use when |
|---|---|---|---|
| **Desktop Mode, raw binary** | `just deploy` → `just run` | the rsynced `content_shell` directly, `--no-sandbox`, `DISPLAY=:1` | fast iteration; no Flatpak, no Steam |
| **Game Mode, Steam shortcut** | `just flatpak` → copy bundle → `just install` *(on the Deck)* → launch from Steam | the sandboxed Flatpak | what users actually get |
| **Game Mode, headless** | …then `just remote-run` *(from the workstation)* | same Flatpak, launched via `steam://rungameid/…` | automation; the on-Deck test harness |

> `just deploy` does **not** pass `--delete` — it preserves a user-fetched Widevine CDM under
> `~/cobalt-yt/`. (The `justfile` comment claimed `--delete` for a while; it was wrong.)
>
> **`just deploy` alone leaves Game Mode stale.** The Steam shortcut runs the *installed Flatpak*,
> not the rsynced bundle. To test a change in Game Mode you must rebuild and reinstall the bundle.

---

## 5. What is NOT implemented

Being explicit, because a missing recipe is indistinguishable from a broken one to an agent.

| Missing | Planned as | Blocks |
|---|---|---|
| `just gdb` | plan §15 — remote gdbserver | on-Deck debugging |
| audio-restored / no-dim checks | the last two clauses of the P6 gate | `just soak` proves two of four |
| a recorded cert baseline | T7d — recorded 2026-07-10 (`conformance-test.json`, 45/46) | L3 adjudication (other suites still exit 3 until recorded) |

**`just test-deck` exists as of 2026-07-09** (T2/T3) and **ran against a Deck for the first time on
2026-07-10** — but that run was blocked by Leanback's "watch as guest" account gate, so it did not
verify the §2 rows (before the fix it also could not even reach the Deck; harness.md F14). Read that
before quoting L2 as coverage: a run that stalls at the account wall proves little more than a
skipping one, until a `conftest.py` fixture dismisses the gate. What *is* proven is its pure
machinery — `tests/deck/lib/`
(uinput struct/ioctl encoding, the power parsers, the decoder verdict, the SSH error taxonomy) is
unit-tested at L0 by `tests/harness/test_deck_lib.py`, which CI runs on every push. That L0 file
already caught two real bugs before any hardware existed: a 16-byte `struct input_event` (with
struct's `=` prefix, `l` is 4 bytes, not 8) and a power parser that would have read an empty sysfs
node as `0.0 W`.

Markers, and what a failure means:

| Marker | Meaning | A failure is |
|---|---|---|
| `gate` | must pass on every run | a **product defect**; blocks the release |
| `probe` | discovery of something listed unverified in TEST-PLAN §2 | a **finding to register**, not a regression — but still reported, never swallowed |
| `uinput` | needs `/dev/uinput` writable on the Deck (udev rule) | usually **environment** (exit 3) |
| `playback` | needs a video playing | environment |

`scripts/test-deck.sh` maps pytest's exit codes onto the taxonomy: a failed test is `2` (the product
is wrong), a missing Deck or missing pytest is `3`, an unreachable configured Deck is `4`. It never
reports a skipped-everything run as success.

Present but **L0-only**: `just launcher test` (12 binaries) and `just test-harness`
(`power_adjudicate`, the `cdp` library, `tests/deck/lib/`, `deckctl`). Both run anywhere in about a
second and are the whole loop for launcher/config/scripts changes.

### `scripts/deckctl.py` — the eye `power` and `soak` were missing (T5, 2026-07-09)

`just power` sampled the battery without knowing whether a video was playing, and `just soak` checked
that a *process* survived resume, which is one of the four clauses of the P6 gate. `deckctl.py` runs
on the workstation, opens its own `ssh -L` tunnel, and answers with an exit code:

| Command | Question | Exit |
|---|---|---|
| `playing --window N` | is `currentTime` advancing? | 0 yes · 3 no video / paused · 2 stalled or **rewound** |
| `current-time` | where is the playhead | 0 · 3 no video |
| `decoder [--require-vaapi]` | which decoder is the engine using | 0 VA-API · 2 software · 3 the Media domain never reported |
| `epp` | `energy_performance_preference` per core | 0 · 3 unreadable |

**`paused` is ENV and `stalled` is ASSERT, and they are the same `currentTime` reading.** Getting
them backwards sends whoever reads the log to debug VA-API because someone forgot to press play — or,
worse, files a real stall as operator error. Likewise a silent `Media` domain is a broken probe (3),
never software decode (2). Backwards movement is named as "the player restarted", not as a stall:
both fail the P6 gate, but only one sends you hunting a hang that never happened.

EPP is **measured and reported, never gated.** SteamOS resets it on cores 1–7 after resume
(steamos#2383) and writing it back needs root, so it is infeasible from the Flatpak. A gate you
cannot pass and cannot fix is a gate someone disables, and then it stops reporting too.

### `just cert` — L3 conformance (T7). Built 2026-07-09; first ran 2026-07-10 (45/46).

Serves the pinned `js_mse_eme` (`tests/cert/HARNESS.pin`) from `127.0.0.1` and drives it over CDP.
The `conformance-test` suite ran for real on 2026-07-10: **45 PASSED, 1 FAILED** (the failure,
`MediaElementEvents`, is a harness property, not our engine — finding F11), and
`tests/cert/expectations/conformance-test.json` records that baseline. The **other** suites still have
no baseline, so `cert.py` exits 3 for them with instructions rather than passing a run it has no
opinion about. Four things it will not do:

- **It never trusts `getTestResults()`.** The harness's own helper buckets tests into `pass`/`fail`
  and silently drops `UNKNOWN` and `TIMEOUT`. A suite wedged on test 3 of 400 reports zero failures.
  We walk `globalRunner.testList` instead, and an `UNKNOWN` **fails the run** — a suite that never
  reached 397 tests has no opinion about them, and silence is not assent.
- **It never runs on an insecure or phone-home origin.** EME needs a potentially-trustworthy origin,
  which `http://localhost` is and a LAN IP is not; and the harness POSTs results to
  `qual-e.appspot.com` when the page URL contains `appspot.com`/`googleapis.com`. Both are asserted
  before the first test, so a misconfigured host fails loudly instead of looking like a decoder bug.
- **It never lets the vectors go unhashed.** `harness/util.js` is rewritten at serve time to point at
  our origin, and the rewrite *raises* if the constants moved — a silent no-op would send all ~244
  vectors back to Google's bucket with the run still green. A hash mismatch exits **3** (the bucket
  changed), never 2 (our decoder broke).
- **It never fails a build on `playbackperf-*`.** TDP, refresh rate, and thermals make it
  nondeterministic on a handheld. Recorded as a trend; pair a drop with the decoder probe to
  attribute it. And it never touches Widevine: CI must not fetch Google's CDM.

Passing `js_mse_eme` is **not** YouTube certification and confers no status (`docs/legal.md`).

`scripts/cdp.py` **is now a library as well as a CLI** (T1, 2026-07-09). Importable surface:
`CDP` (context manager; `call` / `evaluate` / `wait_for` / `pump` / `screenshot` / `set_user_agent`),
`key_spec` (named keys **and** any single printable char, with the `text` field that makes
`beforeinput`/`textInput` fire), `dispatch_key(name, modifiers)`, `dispatch_raw(...)` for the spikes'
**negative controls**, `type_text`, `click` / `mouse`, and `MediaState` — the CDP `Media`-domain
accumulator that feeds the P4 gate. `CDP.call` now feeds unsolicited events to `MediaState` instead
of discarding them; that discard is why decoder identity was previously unobtainable.

`MediaState` **accumulates and does not adjudicate.** The P4 verdict is spelled once, in
`tests/deck/lib/probes.py:hardware_decode_verdict(name, is_platform)`: pass iff
`kIsPlatformVideoDecoder` is true *and* the name contains `Vaapi` (substring — an older path
stringifies as `VaapiVideoDecodeAccelerator`). A **missing** value is a failure, not a pass: it means
the Media domain never reported, and a broken probe must never look like working hardware. Never
scrape `chrome://media-internals` — it is an unstable WebUI over the same MediaLog stream.

The CLI obeys the §1 taxonomy (`2` assertion, `4` transport) and grows three flags for the gate
above: `--add-script FILE` (`Page.addScriptToEvaluateOnNewDocument`, before `--navigate`),
`--baseline EXPR` (must hold *before* injection — a negative control), and `--assert EXPR`. Every
predicate must evaluate to exactly `true`; truthiness is not enough, because `undefined` from a
misspelt property reads as a clean answer under `==` and is not one.

Pure parts are covered by `tests/harness/test_cdp_lib.py`. Do not add Selenium/Playwright: they
violate the minimal-deps rule and cannot run in the build image.

---

## 6. Contract for a new recipe

1. Logic lives in `scripts/<name>.sh`; the `justfile` entry is one delegating line plus a comment.
2. Source `lib.sh`. Use `die_assert` / `die_env` / `die_transport` / `die_usage` — never bare `exit 1`
   for something you can classify.
3. Reach the Deck **only** through `deck_ssh` / `deck_rsync`. Raw `ssh` ignores `STEAMDECK_PORT` and
   the `sshpass` path. (`power.sh` and `soak.sh` used to get this wrong.)
4. **Verify what you claim.** A launch recipe polls until the process *and* its CDP endpoint answer;
   a measurement recipe validates every sample and voids the run rather than averaging over garbage.
   Fire-and-forget plus `exit 0` is a lie. (`remote-run.sh` used to be one.)
5. Never mask a failure with `|| true` / `|| info "skipped"` unless the skip condition was tested
   *separately first*. (`fmt.sh` used to report container crashes as "no checkout yet".)
6. If a human is required, say so in the recipe comment **and** in §3's `H` column.
7. Add L0 coverage to `tests/harness/` for any adjudication logic — anything that decides pass/fail.
   Then break the implementation and confirm the test fails. A test that has never failed has never
   tested anything.
8. Run `shellcheck -S warning scripts/*.sh tests/harness/*.sh` (CI does).

---

## 7. Configuration

`.env` (from `.env.example`) and `.steamdeck_auth` (gitignored) are both sourced by `lib.sh`.

| Var | Used by | Meaning |
|---|---|---|
| `CONTAINER_ENGINE` | all container recipes | `docker` (default) or `podman` |
| `COBALT_SRC` | all tree recipes | gclient root; the engine tree is `$COBALT_SRC/src` |
| `NINJA_JOBS` | `build` | parallelism (16 on a 31 GB box — see memory) |
| `DECK_HOST` | all Deck recipes | `user@ip`; overridden by `STEAMDECK_USER`+`STEAMDECK_IP` |
| `STEAMDECK_PORT` | `deck_ssh` | SSH port (default 22) |
| `STEAMDECK_PASSWORD` | `deck_ssh` | optional; uses `sshpass` when set. Key auth is the norm |
| `DECK_DISPLAY` | `run` / `remote-run` | `:1` for the gamescope Xwayland session, `:0` for Steam |
| `DECKBACK_MAX_WATTS` | `power` | override the ≤9 W gate |
| `DECKBACK_CDP_PORT` | `remote-run` | DevTools port to verify (default 9222) |
| `DECKBACK_EXTRA_ARGS` | `run` | extra engine flags, not baked into `config/app.json` |

Runtime app config is `config/app.json` — remotely overridable by design (doc §6 / R1), so a
server-side Leanback break can be hotfixed without a rebuild.

---

## 8. For an agent: the decision tree

- Changing only `launcher/`, `config/`, or `scripts/`? → `just launcher test` + `just test-harness`.
  **No container, no Deck, seconds.** This is the whole loop for most launcher work.
- Changing engine behavior, GN args, or `patches/`? → `just build dev` then `just smoke`.
- Claiming something works on hardware? → you cannot, from a workstation. Say **"implemented, not
  verified"** and point at `.internal/TEST-PLAN.md` §2. The distinction is enforced culturally here
  and it is the project's most important habit.
- Got a non-zero exit? → **classify it with §1 before debugging.** `3` and `4` are never the product.
