---
scope: durable
title: Page scripts — the ScriptLibrary, and why injected JS is authored as data
created: 2026-07-13
sources:
  - session 2026-07-13 (launcher review → seek feature → JS-injection refactor)
  - launcher/src/scripts.{hpp,cpp}, config/scripts/*.js
---

# Page scripts: one registry for everything the launcher injects into youtube.com/tv

Deckback is a web app in a kiosk shell, so *every* feature that touches the page is JavaScript pushed
over CDP: AV1 steering, touch/pointer suppression, the seek skip, the toast, the play-state poll, the
suspend/resume media nudges, voice ducking + mic-button probe, the error page, the controls card.
This is not a hack to be minimised — it is the platform-layer principle (the launcher is the embedder;
the page is the app). But *how* that JS was authored had drifted into three inconsistent tiers, and
this note records the design that unified them so the next page-side feature is cheap and safe.

## The three tiers that existed, and the one that was wrong

1. **Runtime hot-swap** — `app.json`, read from disk at launch. Ship a new one, no rebuild. This is
   the R1 hotfix surface the design doc keeps invoking.
2. **Build-time embedded `.js`** — `av1_steering.js`, `no_pointer.js`: authored as real, lintable
   `.js` files and embedded into the binary via a CMake codegen. The **good** pattern.
3. **Inline C++ string-literal JS** — `toast_js`, `build_skip_js`, the player exprs, `mic_probe_js`,
   `error_page_js`: hand-built with `std::format`, escaping re-derived at each call site, invisible to
   anyone reading the `.js` files, not hot-swappable. **This tier was the problem.**

Two failure modes recurred in tier 3: escaping bugs (a quote in a toast/selector/title silently
breaks the injected JS, and `eval` failures are swallowed by design — only a test catches it), and
"you cannot fix Leanback breakage without a rebuild" for behaviours that live in the binary.

## The design (`launcher/src/scripts.{hpp,cpp}`)

**Every injected script is a real `.js` file in `config/scripts/`, reached through one registry.**

- **`ScriptParams`** — a JSON object-literal builder. Numbers/bools/strings/`string[]`/`pair[]` are
  encoded through **one** escaper (`util.hpp::js_string_escape`). No call site hand-escapes JS again.
- **`ScriptLibrary`** — a process-wide singleton (`instance()`), populated from the embedded
  registry. Two shapes, matched to use:
  - **One-shot** scripts are a parenthesised function expression `(function(p){ ... })`.
    `render(name, params)` returns `(body)(json)`; `invoke()` eval_void's it. The call site never
    concatenates JS.
  - **Sticky** scripts (document-start, `Page.addScriptToEvaluateOnNewDocument`) are self-invoking
    `(function(){...})();` and `install_sticky()` installs them **verbatim** (no params). `just smoke`
    injects the same file verbatim, so the canary runs identical text.
- **Runtime override** — `load_overrides(dir)` (called once in `main()` before any thread) lets a
  same-named `.js` in the scripts dir shadow the embedded default. This gives injected behaviours the
  same ship-a-fix-without-a-rebuild property `app.json` has, same trust boundary. Dir defaults to
  `<config dir>/scripts`; `DECKBACK_SCRIPTS_DIR` overrides. Loaded once → the map is read-only when
  shared across the input/player/voice threads (no locking needed).
- **CMake** globs `config/scripts/*.js` into `scripts_registry.hpp` (fail-closed: a missing dir/file
  fails the build, never silently ships a disabled behaviour). The flatpak manifest adds the whole
  dir as one `type: dir` source (`-DDECKBACK_SCRIPTS_DIR=scripts`), because flatpak-builder copies a
  module's sources flat.

## How to add a page-side feature now

1. Write `config/scripts/<name>.js` as `(function(p){ ...; return ...; })` (one-shot) or a
   self-invoking IIFE (sticky). Comment it — these files are the one heavily-commented exception to
   the no-comments rule, because the *why* has nowhere else to live.
2. Call `ScriptLibrary::instance().render("<name>", ScriptParams().set("k", v))` (one-shot) or
   `install_sticky(client, "<name>")` (sticky). Pass **raw** values to `ScriptParams` — it escapes
   once. Do NOT `js_string_escape` a value before handing it over (that double-escapes).
3. Add an L0 assertion in `scripts_test.cpp` or the feature's test.

## Gotchas learned the hard way

- **Do not double-escape.** The tier-3 migration's one real trap: the old code escaped values before
  building the JS string; `ScriptParams` escapes them. Pass raw. Structured HTML (the controls-card
  rows) is passed as **data** (`pair[]`), not a pre-built HTML string, so it is escaped exactly once.
- **Test doubles key on substrings.** `tests/fake_cdp_server.hpp` picks its reply from markers in the
  expression text: `/*voice*/` (voice duck, matched before player pause/play), `activeElement`
  (play-state bitmask), `getBoundingClientRect` (mic probe), `pause()`/`play()` (player checkpoint/
  nudge), `documentElement.innerHTML` (error page). A migrated script MUST preserve its marker or the
  fake answers the wrong thing. This is why `voice_*.js` keep `/*voice*/` and `player_*.js` keep the
  bare `pause()`/`play()`.
- **Sticky ≠ one-shot shape.** Sticky scripts self-invoke and install verbatim; a one-shot is an
  uninvoked function `render()` calls with params. `just smoke` injects sticky files verbatim, so a
  sticky file that were authored as an uninvoked `(function(p){...})` would silently do nothing under
  smoke. Keep av1_steering/no_pointer self-invoking.

## Status
Implemented 2026-07-13 across three commits (core+skip+toast+sticky; player+voice; error-page+card).
All migrations behaviour-preserving; `just launcher test` green under -Werror. The hardware-verified
behaviours (AV1 steering, no_pointer touch inertness, toast/card rendering over real Leanback, player
suspend/resume nudges) were preserved verbatim but SHOULD be spot-re-verified on-Deck on the next
build, since the wrapper text around each script changed even though the logic did not.
