---
scope: durable
verified: 2026-07-09
method: read the pinned Cobalt tree (first at the M114 pin a9181df8; the §3 autoplay and decoder-cap links re-checked at the M138 pin a1d34a1a, 2026-10-21) + read our launcher/config; no on-Deck run, no live youtube.com/tv observation
sources:
  - cobalt/src (third_party/blink/renderer/core/html/media/, media/base/, media/gpu/vaapi/)
  - config/scripts/av1_steering.js (AV1 steering), launcher/src/devtools.cpp (CDP surface), config/scripts/player_state.js (single-<video> model)
  - config/app.json (steer_av1_unsupported, user_agent, autoplay policy)
  - .internal/findings/durable/pip.md (the PiP dead-end — a DIFFERENT feature, see below)
  - .internal/findings/durable/input-ux.md (§13.2 voice-search UA-gating precedent; focus vs hover)
  - .internal/findings/durable/hardware.md (VA-API decoder facts, AV1 dispute)
  - .internal/TEST-PLAN.md §0 (assert on Leanback, never on a value we chose), §5 (Media-domain decoder probe)
---

# Focus / hover video preview — feasibility

**Verdict: NOT a dead end, and NOT the same verdict as PiP. There is no engine or architectural
blocker — a muted inline preview `<video>` (or an animated-thumbnail `<img>`) is ordinary page
content that needs no window backend, and the engine allows it (muted autoplay enabled, no per-page
concurrent-player cap). The feature is Leanback's to render, not ours to build, so the only real
questions are (a) whether `youtube.com/tv` exposes a focus/hover preview surface *at all under our
Cobalt UA*, and (b) whether our own AV1 steering or UA is *suppressing* one. Both need the live app
on the Deck.** The finding is **"no blocker found; the crux is a live-app property — here is the probe
that settles it."** TASKS.md Phase 11 is that spike.

**Read this alongside `durable/pip.md` — different features, different verdicts.** PiP is dead
because it needs a browser *PiP window backend* the shell stubs out (`kNotSupported`), a second
top-level surface gamescope Game Mode won't host, and a second stream Leanback won't yield.
**Preview shares none of those blockers**: it is in-page content in our one surface, it is (at most)
a *second decode* rather than a second window, and it is *Leanback's own* element.

Everything below is *verified by reading the tree/our code* except where marked an **unverified
assumption** with its on-Deck probe. Nothing here was run on a Deck or observed on the live site.

## 1. What is YouTube TV's preview mechanism? — UNKNOWABLE from here (Q1)

It cannot be determined without observing the live app on-device: the mechanism lives entirely in
Google's server-delivered, obfuscated `ytlr-*` bundle, not in `cobalt/src` or anything we ship. The
candidates:

- **(a) an animated WebP/GIF/sprite thumbnail** swapped into the focused tile's `<img>` — a
  `currentSrc` change on an `<img>`, *no* `<video>`, *no* MSE, and **not affected by our AV1
  steering** (an `<img>` never calls `isTypeSupported`/`canPlayType`/`decodingInfo`);
- **(b) a muted inline `<video>` / MSE "hover preview"** — a new `<video>` element (likely
  `src="blob:…"` if MSE) becoming `paused=false, muted=true`;
- **(c) a DASH low-res representation** — a special case of (b), same observable signature;
- **(d) nothing at all on the TV surface** — the 10-foot Leanback layout may simply not carry the
  desktop/mobile hover-preview product.

These have **distinct, cheap observable signatures**, so the probe below settles it in one on-Deck
session. Known from our code: the player scripts assume a **single**
`document.querySelector('video')` (singular) is the whole play-state model (`player_state.js` and the
other player scripts), so a preview that adds a *second* concurrent `<video>` would break that
assumption — a code consequence, not a blocker.

### ★ The Q1 probe (ready-to-run artifact — the deliverable)

Run over CDP (`Runtime.evaluate`, `returnByValue:true`) against `youtube.com/tv` **on the Deck**,
after dismissing the account gate (a fresh profile rewrites all routes to `#/` until
`ArrowDown`,`Enter` clears "Watch as guest") and navigating focus onto the browse grid. Capture it
**three times** — (i) before focus is on a tile, (ii) immediately after D-pad focus lands on a video
tile, (iii) after a ~3 s dwell on that tile — and **diff the three**. A new `<video>` appearing =
candidate (b)/(c); an `<img>` `currentSrc` changing to an animated asset = (a); no change = (d) or
hover-gated (see §4).

```js
(function(){
  var f = document.activeElement;
  function rect(el){ if(!el||!el.getBoundingClientRect) return null;
    var r=el.getBoundingClientRect();
    return {x:Math.round(r.x),y:Math.round(r.y),w:Math.round(r.width),h:Math.round(r.height)}; }
  var allVids = document.querySelectorAll('video');
  var videos = [].map.call(allVids, function(v){
    return { src:(v.currentSrc||v.src||'').slice(0,80),
             mse:(v.currentSrc||'').indexOf('blob:')===0,
             paused:v.paused, muted:v.muted, ended:v.ended,
             vw:v.videoWidth, vh:v.videoHeight, readyState:v.readyState,
             autoplay:v.autoplay, loop:v.loop, rect:rect(v) }; });
  // <img> assets inside the focused subtree — catches the animated-thumbnail path (a)
  var scope = f && f.querySelectorAll ? f : document;
  var imgs = [].slice.call(scope.querySelectorAll('img')).slice(0,8).map(function(i){
    return { src:(i.currentSrc||i.src||'').slice(0,120),
             nw:i.naturalWidth, nh:i.naturalHeight, rect:rect(i) }; });
  return JSON.stringify({
    ua: navigator.userAgent,
    h5vcc: typeof window.h5vcc,                 // §2: is a Starboard/platform bridge present?
    videoCount: allVids.length,
    videos: videos,
    focus: { tag:f&&f.tagName,
             cls:((f&&f.className)||'').toString().slice(0,120),
             aria:(f&&f.getAttribute&&(f.getAttribute('aria-label')||''))||'',
             // any data-* that names a preview surface (do not assume names; just dump them)
             data:(function(){var o={};if(f&&f.attributes)for(var i=0;i<f.attributes.length;i++){
                    var a=f.attributes[i]; if(a.name.indexOf('data-')===0) o[a.name]=a.value.slice(0,40);}
                    return o;})() },
    focusedImgs: imgs
  });
})()
```

Assert on **Leanback's** state (TEST-PLAN §0): the signal is a `<video>`/`<img>` that *appears or
changes* on focus, never a value we chose. If a preview `<video>` appears, chain the **§5 Media-domain
decoder probe** on its `playerId` to learn the codec and decode path.

## 2. Is OUR configuration suppressing it? — the highest-value question (Q2)

As with voice search (input-ux §13.2), **our own identity/steering bets are the most likely thing to
break a feature that would otherwise work.** Three candidate suppressors, graded:

### 2a. AV1 steering — a real HYPOTHESIS, not verified, and narrow

`config/scripts/av1_steering.js` (a sticky document-start script) overrides **three** APIs —
`MediaSource.isTypeSupported`, `HTMLMediaElement.prototype.canPlayType`, and
`navigator.mediaCapabilities.decodingInfo` — to report unsupported for any codec string matching
`/av01|(^|[^a-z])av1([^a-z]|$)/i`. It catches `av01…` and bare `av1` and leaves `vp9`/`vp09`/`avc1`
untouched (the `avc1`-not-matched-as-`av1` case is tested). Shipped default is
**`steer_av1_unsupported: true`**.

**If — and only if — the preview is a muted MSE `<video>` whose codec is AV1, our steering makes that
representation report unsupported and Leanback would have to fall back or serve nothing.** Short
low-res clips are a *plausible* AV1-only surface. Bounded by three facts:

- **It does not touch candidate (a).** An image-based preview never queries those APIs.
- **It does not reach workers.** `Page.addScriptToEvaluateOnNewDocument` does not run in worker
  contexts, and Chromium supports MSE-in-Workers. If Leanback probes preview codecs off the main
  thread, steering is **bypassed** and is *not* the suppressor.
- **It is a config toggle** — patch-free to test (the §2 A/B below).

### 2b. The Cobalt TV UA — a real HYPOTHESIS (same shape as voice search)

We ship `Mozilla/5.0 (ChromiumStylePlatform) Cobalt/26.lts.0,gzip(gfe)` (`config/app.json`
`user_agent`). Leanback may gate a preview on a platform capability token, or route it through an
`h5vcc`/Starboard service this Chromium-based build **does not have** (zero Starboard symbols in the
binary; input-ux §13.2 is exactly this failure for voice). Two ways our UA could hide preview: (i) the
"Cobalt living-room device" bundle may not carry the preview product; (ii) it may render a preview
whose handler calls a platform API absent here. The §1 probe dumps `typeof window.h5vcc` and the full
UA, so the first capture tells us which bundle we got.

**Probe:** compare the §1 enumeration under our Cobalt UA vs a different TV UA (PS4 / Tizen /
generic-Cobalt UAs also reached interactive Leanback — `../milestones/m138.md` §Leanback UA). Preview
under one UA and not ours ⇒ the UA is the suppressor.

### 2c. `mediaCapabilities.decodingInfo` answers — subsumed by 2a

The only `decodingInfo` answer we alter is the AV1 one; for VP9/H.264 the script returns the real
implementation's answer. There is no separate decodingInfo suppressor beyond AV1.

### ★ The Q2 probe (A/B, patch-free)

```
# Session A — shipped config (steer_av1_unsupported:true, Cobalt UA): run the §1 probe, 3 captures.
# Session B — flip config/app.json steer_av1_unsupported:false, relaunch: run the §1 probe again.
# Session C — keep steering, swap UA (e.g. a PS4/Tizen TV string via cdp.py --ua): re-probe.
# A preview <video> that appears only in B  => AV1 steering suppresses it (verified).
# A preview surface that appears only in C  => our Cobalt UA suppresses it (verified).
# Identical across A/B/C, with a preview present => our config is NOT the suppressor.
# Nothing in any of A/B/C                       => candidate (d): Leanback has no TV-surface preview.
```

## 3. Can the engine even do it? — YES, no embedder gap (Q3)

Unlike PiP, an inline muted `<video>` (or an `<img>`) needs **no** window backend:

- **Muted autoplay is allowed.** We pass `--autoplay-policy=no-user-gesture-required`
  (`config/app.json` `cobalt_flags`), which maps to `AutoplayPolicy::Type::kNoUserGestureRequired`.
  Under that policy autoplay is permitted for *all* videos regardless of muted state —
  `AutoplayPolicy::DocumentShouldAutoplayMutedVideos` returns **false** precisely because the policy
  already allows everything (`third_party/blink/renderer/core/html/media/autoplay_policy.cc`). A
  `<video autoplay muted>` preview plays with no user gesture.
- **No per-page concurrent-player cap.** No `kMaxWebMediaPlayers` / player-count limit exists in
  `content/browser/media/` or `third_party/blink/renderer/`, and no background-player pause/throttle
  in `content/browser/media/`.
- **The only ceiling is the VA-API decoder cap, and it is not binding.** At M138
  `VaapiWrapper::GetMaxNumDecoderInstances()` (`media/gpu/vaapi/vaapi_wrapper.cc`) allows **16**
  concurrent decoders (10 only on AMD Stoney Ridge) while `kLimitConcurrentDecoderInstances` is on
  (enabled by default) — the same cap `pip.md` §3 cites.

The three layers:
- **ENGINE:** permits it. No blocker.
- **PAGE (Leanback):** owns whether a preview surface exists under our UA — **the crux** (§1/§2).
- **HARDWARE:** whether radeonsi/VCN serves a *second* simultaneous `VAEntrypointVLD` context is
  unmeasured (`pip.md` §3) — but a browse-grid preview usually runs when **no main video is
  playing**, so it is typically a *single* decode, and this unknown may never be exercised.

## 4. Does the Deck's input model even produce a "hover"? (Q4)

On a TV there is a **focused tile**; on our build the D-pad moves focus (CDP-dispatched arrows move
`document.activeElement`, trusted). Two branches:

- **Focus-triggered** (the norm for a 10-foot app — remotes have no pointer): our D-pad focus
  **already produces the trigger**; zero input work. Likely for a TV surface, but a *reasoned
  expectation, not verified*.
- **Pointer-hover-gated** (a desktop/mobile idiom in the bundle): by default **we have no hover** —
  Game Mode gives Leanback key events, not a moving cursor. Optional pointers exist —
  `Input.dispatchMouseEvent` in `devtools.cpp` (`dispatch_mouse` / `mouse_press` / `mouse_click`), the
  right trackpad (`config/steam_input.vdf`, input-ux §7) — but none tracks focus, and with
  `disable_touch` on (the default) `no_pointer.js` swallows pointer/mouse events in the page
  (`touch-lock.md`). A hover-gated preview would need a synthesized `mouseMoved` at the focused tile's
  rect on every focus change — buildable (CDP mouse coords are CSS-viewport pixels, input-ux §13.1),
  but a real code path.

**The probe:** if the preview appears on D-pad focus alone (§1 step ii), it is focus-triggered; if
only after `Input.dispatchMouseEvent {type:"mouseMoved"}` at the focused tile's
`getBoundingClientRect()` centre, it is hover-gated. **Unverified assumption + the probe that settles
it.**

## 5. Cost (Q5)

**If** preview is a muted low-res inline video, the cost is a *second decode* — usually **not
concurrent** with a main stream, because focus-preview happens while browsing the grid. The concurrent
case only arises if Leanback previews a tile *while the player is open*, which the §1 probe
(`videoCount`) reveals.

No preview wattage exists in-repo. The P4 baseline is measured (single-stream VP9 under hardware
decode, ≈5.6–5.9 W, OLED — `hardware.md`). What must be measured:

- **Decoder identity of the preview** via the TEST-PLAN §5 CDP `Media`-domain probe:
  `kVideoDecoderName` / `kIsPlatformVideoDecoder` on the preview `playerId`. `Dav1dVideoDecoder` = AV1
  in **software** (worst case for battery, and evidence steering was bypassed per §2a);
  `VaapiVideoDecoder` = hardware.
- **Power delta** via `just power`: `power_now` with preview active vs the grid idle. OLED-only; never
  transfers to LCD (`hardware.md`).

## 6. Verdict + what would have to change

**No engine or architectural blocker — so, unlike PiP, NOT a dead end.** Whether the feature exists
under our UA, and whether our steering suppresses it, is a **live-app property**. It reduces to **one
on-Deck spike**: the §1 enumeration probe (pre-focus, on-focus, on-dwell) plus the §2 A/B. Outcomes:

| §1/§2 probe outcome | Meaning | Next |
|---|---|---|
| A preview `<video>`/`<img>` appears on **focus** under our config | **It already works, free.** Verify decode cost (§5). | Document; maybe handle play-state confusion over a 2nd `<video>` (§1). |
| Preview appears only with `steer_av1_unsupported:false` (§2a) | AV1 steering suppresses an AV1-only preview. | A **config decision**: accept no preview, or narrow the steering to let short/low-res AV1 through (a script change, patch-free). Steering stays default-on for LCD safety (`hardware.md`), so likely *accept no preview*. |
| Preview appears only under a **different UA** (§2b) | Our Cobalt UA hides it (voice-search shape). | Likely accept: the UA is the R1 identity bet the project rests on. |
| Preview appears only after a synthetic **hover** (§4) | Hover-gated; we have no hover by default. | A launcher feature: synthesize `mouseMoved` at the focused tile on focus change. |
| **Nothing** appears in any variant (candidate (d)) | **Leanback has no TV-surface preview.** | Dead end at the *page* layer, not the engine. Register and stop. |

**In no branch is a `patches/` engine change required** — the sharpest contrast with PiP, which needs
a chrome-layer PiP-window backend ported into the shell.

### Open questions (each needs the OLED Deck + the live site)
- Does `youtube.com/tv` expose a focus/hover preview surface **under our Cobalt UA** at all? (§1, §2b)
- If it does, is it (a) an animated `<img>`, or (b)/(c) a muted inline/MSE/DASH `<video>`? (§1)
- Is our **AV1 steering** suppressing an AV1-only preview? (§2a — config A/B)
- Is it **focus**-triggered (works today) or **pointer-hover**-gated (needs a synthesizer)? (§4)
- If it is a video, is it hw (`VaapiVideoDecoder`) or sw (`Dav1dVideoDecoder`) decoded, and what does
  it cost against the P4 baseline? (§5)
