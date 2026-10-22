---
scope: durable
title: Page scripts — the ScriptLibrary, and why injected JS is authored as data
created: 2026-07-13
sources:
  - launcher/src/scripts.{hpp,cpp}, config/scripts/*.js
  - launcher/tests/fake_cdp_server.hpp, launcher/tests/scripts_test.cpp
---

# Page scripts: one registry for everything the launcher injects into youtube.com/tv

Deckback is a web app in a kiosk shell, so *every* feature that touches the page is JavaScript pushed
over CDP: AV1 steering, touch/pointer suppression, the touch-gesture router, seek/skip, the toast,
the OSD, the play-state poll, the suspend/resume media nudges, the error page, the controls card.
This is the platform-layer principle (the launcher is the embedder; the page is the app), not a hack
to be minimised.

**Every injected script is a real `.js` file in `config/scripts/`, reached through one registry.**
No call site builds JS out of C++ string literals: that pattern produced escaping bugs (a quote in a
toast/selector/title silently breaks the injected JS, and `eval` failures are swallowed by design)
and put page behaviour out of reach of a no-rebuild fix.

## The design (`launcher/src/scripts.{hpp,cpp}`)

- **`ScriptParams`** — a JSON object-literal builder. Numbers/bools/strings/`string[]`/`pair[]` are
  encoded through **one** escaper (`util.hpp::js_string_escape`). No call site hand-escapes JS.
- **`ScriptLibrary`** — a process-wide singleton (`instance()`), populated from the embedded
  registry. Two shapes, matched to use:
  - **One-shot** scripts are a parenthesised function expression `(function(p){ ... })`.
    `render(name, params)` returns `(body)(json)`; `invoke()` eval_void's it. The call site never
    concatenates JS.
  - **Sticky** scripts (document-start, `Page.addScriptToEvaluateOnNewDocument`) are self-invoking
    `(function(){...})();` and `install_sticky()` installs them **verbatim** (no params). `just smoke`
    injects the same file verbatim, so the canary runs identical text.
- **Runtime override** — `load_overrides(dir)` (called once in `main()` before any thread) lets a
  same-named `.js` in the scripts dir shadow the embedded default: the same ship-a-fix-without-a-
  rebuild property `app.json` has, with the same trust boundary. Dir defaults to
  `<config dir>/scripts`; `DECKBACK_SCRIPTS_DIR` overrides. Loaded once, so the map is read-only
  when shared across threads (no locking needed).
- **CMake** globs `config/scripts/*.js` into `scripts_registry.hpp` (fail-closed: a missing dir/file
  fails the build, never silently ships a disabled behaviour). The flatpak manifest adds the whole
  dir as one `type: dir` source (`-DDECKBACK_SCRIPTS_DIR=scripts`), because flatpak-builder copies a
  module's sources flat.

## How to add a page-side feature

1. Write `config/scripts/<name>.js` as `(function(p){ ...; return ...; })` (one-shot) or a
   self-invoking IIFE (sticky).
2. Call `ScriptLibrary::instance().render("<name>", ScriptParams().set("k", v))` (one-shot) or
   `install_sticky(client, "<name>")` (sticky). Pass **raw** values to `ScriptParams` — it escapes
   once. Do NOT `js_string_escape` a value before handing it over (that double-escapes).
3. Add an L0 assertion in `scripts_test.cpp` or the feature's test.
4. Style through CSSOM (`el.style.setProperty`) or `adoptedStyleSheets` only, and build HTML through
   the Trusted Types policy: youtube.com/tv's CSP drops `<style>` and `style` attributes, and a bare
   `innerHTML` throws (`../milestones/m138.md` §"youtube.com/tv page constraints").

## Gotchas

- **Do not double-escape.** `ScriptParams` escapes; pass raw values. Structured HTML (the
  controls-card rows) is passed as **data** (`pair[]`), not a pre-built HTML string, so it is escaped
  exactly once.
- **Test doubles key on substrings.** `tests/fake_cdp_server.hpp` picks its reply from markers in the
  expression text — e.g. `g.drain()` (gesture queue), `/*osd-button-state*/`, `/*overlay-state*/`,
  the OSD's `"op":"state"`/`"cmd"`/`"open"`/`"close"`, `activeElement` (the play-state bitmask),
  `__deckback_error` / `documentElement.innerHTML` (error page), and bare `pause()`/`play()` (player
  checkpoint/nudge). A changed script MUST preserve its marker or the fake answers the wrong thing.
- **Sticky ≠ one-shot shape.** `just smoke` injects sticky files verbatim, so a sticky file authored
  as an uninvoked `(function(p){...})` would silently do nothing under smoke. Keep
  `av1_steering.js`/`no_pointer.js` self-invoking.

## Status

Implemented; `just launcher test` covers the registry, the escaper and each script's shape. On-Deck
verification of individual behaviours is tracked per feature in `.internal/TEST-PLAN.md` §2.
