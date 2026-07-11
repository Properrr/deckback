---
scope: durable
verified: 2026-07-09
method: read the pinned Cobalt tree (cobalt/src @ DEPS.pin a9181df8) + read our launcher/config; no on-Deck run, no live youtube.com/tv observation
sources:
  - cobalt/src (third_party/blink/renderer/core/html/media/, media/base/, media/gpu/vaapi/)
  - launcher/src/navigator.cpp (AV1 steering), launcher/src/devtools.cpp (CDP surface), launcher/src/player.cpp (single-<video> model)
  - config/app.json (steer_av1_unsupported, UA)
  - .internal/findings/durable/pip.md (the PiP dead-end — a DIFFERENT feature, see below)
  - .internal/findings/durable/input-ux.md (§13.2 voice-search UA-gating precedent; focus vs hover)
  - .internal/findings/milestones/m114.md (VA-API decoder facts, AV1 dispute, steering worker-bypass caveat)
  - .internal/TEST-PLAN.md §0 (assert on Leanback, never on a value we chose), §5 (Media-domain decoder probe)
---

# Focus / hover video preview — feasibility

**Verdict: NOT a dead end, and NOT the same verdict as PiP. There is no engine or architectural
blocker — a muted inline preview `<video>` (or an animated-thumbnail `<img>`) is ordinary page
content that needs no window backend, and the engine allows it (muted autoplay enabled, no per-page
concurrent-player cap). The feature is therefore Leanback's to render, not ours to build — so the
only real questions are (a) whether `youtube.com/tv` exposes a focus/hover preview surface *at all
under our Cobalt UA*, and (b) whether our own AV1 steering or UA is *suppressing* one. Both are
unknowable from a workstation and unobservable without loading the live app on the Deck.** So this
finding does not conclude "viable" or "impossible"; it concludes **"no blocker found; the crux is a
live-app property — here is the probe that settles it."** That is the deliverable.

**Read this alongside `durable/pip.md` — they are different features with different verdicts.** PiP
is dead because it needs a browser *PiP window backend* the shell stubs out (`kNotSupported`) plus a
second top-level surface gamescope Game Mode won't host plus a second stream Leanback won't yield.
**Preview shares none of those blockers**: it is in-page content in the one surface we already have,
it is (at most) a *second decode* rather than a second window, and it is *Leanback's own* element,
not one we have to inject. Do not let the PiP verdict transfer — it does not.

Everything below is *verified by reading the tree/our code* except where explicitly marked an
**unverified assumption** with its on-Deck probe. Nothing here was run on a Deck and nothing was
observed on the live site.

## 1. What is YouTube TV's preview mechanism? — UNKNOWABLE from here (Q1)

**Honest answer: it cannot be determined without observing the live app on-device, and I will not
fabricate knowledge of YouTube's private Leanback client code.** The candidates the task names are
all plausible and the tree cannot disambiguate them, because the mechanism lives entirely in
Google's server-delivered, obfuscated `ytlr-*` bundle — not in anything in `cobalt/src` and not in
anything we ship:

- **(a) an animated WebP/GIF/sprite thumbnail** swapped into the focused tile's `<img>` on focus —
  would appear as a `currentSrc` change on an `<img>`, *no* `<video>`, *no* MSE, and (crucially) is
  **not affected by our AV1 steering** (an `<img>` never calls `isTypeSupported`/`canPlayType`/
  `decodingInfo`);
- **(b) a muted inline `<video>` / MSE "hover preview"** — would appear as a new `<video>` element
  (likely `src="blob:…"` if MSE) becoming `paused=false, muted=true`;
- **(c) a DASH low-res representation** — a special case of (b), same observable signature;
- **(d) nothing at all on the TV surface** — entirely possible; the 10-foot Leanback layout may
  simply not carry the desktop/mobile hover-preview product.

These have **distinct, cheap observable signatures**, which is exactly why the probe below settles it
in one on-Deck session. What is *known from the tree* is only the negative space: our `player.cpp`
assumes a **single** `document.querySelector('video')` (singular) is the whole play-state model
(`launcher/src/player.cpp:34,43,48`), so if a preview ever adds a *second* concurrent `<video>` we
would be composing our play-state/idle-inhibit logic over an assumption that no longer holds — a
code consequence worth noting, not a blocker.

### ★ The Q1 probe (ready-to-run artifact — the deliverable)

Run over CDP (`Runtime.evaluate`, `returnByValue:true`) against `youtube.com/tv` **on the Deck**,
after dismissing the account gate (m114.md: fresh profile rewrites all routes to `#/` until
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

Assert on **Leanback's** state (per TEST-PLAN §0): the signal is a `<video>`/`<img>` that *appears or
changes* on focus, never a value we chose. If a preview `<video>` appears, chain the **§5 Media-domain
decoder probe** on its `playerId` to learn the codec and decode path (below).

## 2. Is OUR configuration suppressing it? — the highest-value question (Q2)

This mirrors the voice-search finding (input-ux §13.2): **our own identity/steering bets are the most
likely thing to break a feature that would otherwise work.** Three candidate suppressors, graded:

### 2a. AV1 steering (`navigator.cpp`) — a real HYPOTHESIS, not verified, and narrow

`kAv1SteeringScript` (`launcher/src/navigator.cpp:17-44`) installs, before the first document, an
override of **three** APIs — `MediaSource.isTypeSupported`, `HTMLMediaElement.prototype.canPlayType`,
and `navigator.mediaCapabilities.decodingInfo` — that returns unsupported for any codec string
matching the regex `/av01|(^|[^a-z])av1([^a-z]|$)/i` (line 18). So it catches `av01…` and bare `av1`,
leaves `vp9`/`vp09`/`avc1` untouched (the `avc1`-not-mismatched-as-`av1` case is explicitly tested,
m114.md P4). Shipped default is **`steer_av1_unsupported: true`** (`config/app.json:11`).

**If — and only if — the preview is a muted MSE `<video>` whose codec is AV1 (candidate (b)/(c) with
an AV1 representation), our steering makes that representation report unsupported and Leanback would
have to fall back or serve nothing.** Short low-res clips are a *plausible* AV1-only surface, so this
is a live hypothesis. But it is bounded by three facts:

- **It does not touch candidate (a).** An animated-WebP/GIF/sprite `<img>` never queries those three
  codec APIs, so steering cannot suppress an image-based preview at all.
- **It does not reach workers.** `Page.addScriptToEvaluateOnNewDocument` does not run in worker
  contexts, and M114 supports MSE-in-Workers (m114.md, "AV1-steering caveat"). If Leanback probes
  preview codecs off the main thread, the steering is **bypassed** and is *not* the suppressor.
- **It is a config toggle** — patch-free to test. Flip `steer_av1_unsupported` false and re-run the
  §1 probe; if a preview `<video>` appears only with steering off, steering is the verified suppressor.

**Grade: hypothesis. Probe: the A/B in §2's probe below.**

### 2b. The Cobalt TV UA — a real HYPOTHESIS (same shape as voice search)

We ship `Mozilla/5.0 (ChromiumStylePlatform) Cobalt/26.lts.0,gzip(gfe)` (`config/app.json:5`).
Leanback may gate a hover/focus-preview on a platform capability token, or route it through an
`h5vcc`/Starboard service this Chromium-based build **does not have** (S0.5: zero starboard symbols
in the binary; input-ux §13.2 established exactly this failure mode for voice). Two ways our UA could
hide preview: (i) the TV bundle for a "Cobalt living-room device" may simply not carry the preview
product that the desktop/mobile bundles do; (ii) it may render a preview element whose handler calls
a platform API absent here (dead element). The §1 probe already dumps `typeof window.h5vcc` and the
full UA so the very first capture tells us which bundle we got.

**Grade: hypothesis. Probe: compare the §1 enumeration under our Cobalt UA vs a different TV UA
(the m114.md UA matrix already has PS4 / Tizen / generic-Cobalt strings that all reach interactive
Leanback) — if preview surfaces appear under one UA and not ours, the UA is the suppressor. This is
the direct analogue of the voice-search `h5vcc` check (input-ux §13.2).**

### 2c. `mediaCapabilities.decodingInfo` answers — subsumed by 2a

The only `decodingInfo` answer we alter is the AV1 one (2a). For VP9/H.264 we call the real
implementation (`navigator.cpp:37-40` returns `di(cfg)` for non-AV1), so we do not give YouTube a
"no preview" signal for those codecs. There is no *separate* decodingInfo suppressor beyond AV1.

**Grade: not a suppressor for VP9/H.264 (verified by reading `navigator.cpp`); for AV1 it is the same
lever as 2a.**

### ★ The Q2 probe (A/B, patch-free)

```
# Session A — shipped config (steer_av1_unsupported:true, Cobalt UA): run the §1 probe, 3 captures.
# Session B — flip config/app.json steer_av1_unsupported:false, relaunch: run the §1 probe again.
# Session C — keep steering, swap UA (e.g. the m114.md PS4/Tizen TV string via cdp.py --ua): re-probe.
# A preview <video> that appears only in B  => AV1 steering suppresses it (verified).
# A preview surface that appears only in C  => our Cobalt UA suppresses it (verified).
# Identical across A/B/C, with a preview present => our config is NOT the suppressor.
# Nothing in any of A/B/C                       => candidate (d): Leanback has no TV-surface preview.
```

## 3. Can the engine even do it? — YES, no embedder gap (Q3)

Unlike PiP, an inline muted `<video>` (or an `<img>`) is ordinary page content and needs **no**
window backend. I re-verified there is no equivalent embedder gap:

- **Muted autoplay is allowed.** We pass `--autoplay-policy=no-user-gesture-required`
  (`config/app.json:48`), which maps to `AutoplayPolicy::Type::kNoUserGestureRequired`
  (`media/base/media_switches.cc:44,254,1333-1343` → the flag; `autoplay_policy.cc:47,68-77`). Under
  that policy autoplay is permitted for *all* videos regardless of muted state — and note
  `AutoplayPolicy::DocumentShouldAutoplayMutedVideos` returns **false** precisely because the policy
  *already* allows everything (`autoplay_policy.cc:143-147`): there is no *separate* muted-autoplay
  gate left to trip. So a `<video autoplay muted>` preview would play with no user gesture. **Verified
  by reading the tree.**
- **No per-page concurrent-player cap in this tree.** Grepping `content/browser/media/` and
  `third_party/blink/renderer/` for `kMaxWebMediaPlayers` / "maximum number of players" / a
  player-count limit found **nothing** — the only "limit" hit is an unrelated DevTools inspector
  event-size cap (`inspector_media_context_impl.h:81`). No background-player pause/throttle in
  `content/browser/media/`. So the engine does not block a second concurrent `<video>` element.
  **Verified by reading the tree.**
- **The only real ceiling is the VA-API decoder cap, and it is not binding here.**
  `VaapiVideoDecoder::kMaxNumOfInstances = 16` (`media/gpu/vaapi/vaapi_video_decoder.h:207`); the
  guard `media/gpu/vaapi/vaapi_video_decoder.cc:113-119` (feature `kLimitConcurrentDecoderInstances`,
  `FEATURE_ENABLED_BY_DEFAULT` at `media/base/media_switches.cc:1115`) refuses only the **17th**
  instance. Re-verified myself; same number pip.md cites. One or two concurrent VA-API decodes are
  not software-capped.

**So the constraint is NOT the engine.** Distinguishing the three layers sharply (the task's Q3 ask):
- **ENGINE:** permits it (muted autoplay on; no player cap; ≤16 VA-API decoders). No blocker.
- **PAGE (Leanback):** owns whether a preview surface exists at all under our UA — **the crux, and
  unknowable from here** (§1/§2).
- **HARDWARE:** whether radeonsi/VCN serves a *second* simultaneous `VAEntrypointVLD` context is a
  driver fact we cannot read out of the tree (same open unknown as pip.md §3) — but see §4/§5: a
  browse-grid preview usually runs when **no main video is playing**, so it is typically a *single*
  decode, not concurrent, and this unknown may never be exercised.

## 4. Does the Deck's input model even produce a "hover"? (Q4)

On a TV there is a **focused tile**; on our build the D-pad moves focus (S0.6: CDP-dispatched arrows
move `document.activeElement`, trusted). Whether that is enough depends on how Leanback gates the
preview, and this is a genuine open question with two branches:

- **If preview is focus-triggered** (the norm for a 10-foot app — TV remotes have no pointer, so a
  well-built Leanback preview must trigger on *focus*, not pointer hover), then our D-pad focus
  **already produces the trigger** and the feature would work with zero input work from us. This is
  the likely case for a TV surface, but it is a *reasoned expectation, not verified*.
- **If preview is pointer-hover-gated** (a desktop/mobile idiom leaking into the bundle), then by
  default **we have no hover** — Game Mode gives Leanback synthetic key events, not a moving cursor.
  We *do* have optional pointers: `Input.dispatchMouseEvent` exists in `devtools.cpp` (`dispatch_mouse`
  / `mouse_press` / `mouse_release` / `mouse_click`, lines 766-787), the right trackpad can be an
  optional pointer (`config/steam_input.vdf`, input-ux §7), and the touchscreen emulates a pointer.
  But none of these tracks focus automatically, and a hover-gated preview with no hover **is itself
  the finding**: we would have to synthesize a `mouseMoved` at the focused tile's rect on every focus
  change — buildable (CDP mouse coords are CSS-viewport pixels, no letterbox transform, per input-ux
  §13.1) but a real code path, not free.

**The §1 probe's step (ii) vs a hover variant settles which branch we are in:** if the preview
appears on D-pad focus alone, it is focus-triggered; if it appears only after a synthetic
`Input.dispatchMouseEvent {type:"mouseMoved"}` at the focused tile's `getBoundingClientRect()`
centre, it is hover-gated. **Unverified assumption + the probe that settles it.**

## 5. Cost (Q5)

**If** preview is a muted low-res inline video (candidate (b)/(c)), the cost question is a *second
decode* — but usually **not concurrent** with a main stream, because focus-preview happens while
*browsing the grid*, where no main `<video>` is playing. So the typical case is a **single** low-res
decode, cheaper than PiP's hypothetical "main + secondary." The concurrent case only arises if
Leanback previews an "up next" tile *while the player is open* — which the §1 probe (`videoCount`)
will reveal.

**No wattage number can be invented, and none exists in-repo to reuse.** The P4 budget is
1080p VP9 ≤ ~9 W hw-decode, an **OLED-measured target that has itself NEVER been closed-loop
measured** — `power.sh` still prints "play a 1080p VP9 video now" and trusts a human (TEST-PLAN §E,
§2 C-list, harness finding). There is no baseline to add a preview decode to. **What must be
measured, and with which recipe:**

- **Decoder identity of the preview**, via the TEST-PLAN §5 CDP `Media`-domain probe (T4): assert
  `kVideoDecoderName` / `kIsPlatformVideoDecoder` on the preview `playerId`. `Dav1dVideoDecoder` on
  the preview = AV1 decoded in **software** (worst case for battery, and evidence the steering was
  bypassed per §2a); `VaapiVideoDecoder` = hardware.
- **Power delta**, via `just power` (T5) — but only *after* the gate is made closed-loop (it is not
  today). Sample `power_now` with preview active vs the grid idle. This is an OLED-only number and
  never transfers to LCD (hardware.md; there is no LCD unit).

Both are OLED-Deck-only and cannot be closed on a workstation.

## 6. Verdict + what would have to change

**No engine or architectural blocker exists — so, unlike PiP, this is NOT a dead end.** But the
feature is Leanback's own behaviour, and whether it exists under our Cobalt UA (and whether our AV1
steering suppresses it) is a **live-app property we cannot observe from here.** The whole question
therefore reduces to **one on-Deck spike**: run the §1 enumeration probe (three captures: pre-focus,
on-focus, on-dwell) and the §2 A/B, against `youtube.com/tv` on the OLED Deck. Its outcome forks:

| §1/§2 probe outcome | Meaning | Next |
|---|---|---|
| A preview `<video>`/`<img>` appears on **focus** under our config | **It already works, free.** Verify decode cost (§5); done. | Document; maybe suppress our own play-state confusion over a 2nd `<video>` (§1). |
| Preview appears only with `steer_av1_unsupported:false` (§2a) | AV1 steering suppresses an AV1-only preview. | A **config decision**, not a build: accept no-preview, or a narrower steering that lets short/low-res AV1 through — patch-free (it is a `navigator.cpp` regex/scope change). But re-read hardware.md: steering stays default-on for LCD-safety regardless, so this likely means *accept no preview* on the shipped default. |
| Preview appears only under a **different UA** (§2b) | Our Cobalt UA hides it (voice-search shape). | Likely accept: the UA is the R1 identity bet the whole project rests on; do not trade it for previews. |
| Preview appears only after a synthetic **hover** (§4) | Hover-gated; we have no hover by default. | A launcher feature: synthesize `mouseMoved` at the focused tile on focus change (buildable, real code). |
| **Nothing** appears in any variant (candidate (d)) | **Leanback has no TV-surface preview.** | Dead end — but for a *page* reason (the product isn't there), not an *engine* reason. Register and stop. |

**For preview to become a shipped feature, at most one of these must be true and true only on-Deck:**
(1) Leanback already renders a focus-preview under our UA — then nothing to build; or (2) it renders
one that our config suppresses and we choose to un-suppress (config-only, patch-free); or (3) it is
hover-gated and we add a focus→`mouseMoved` synthesizer in `launcher/` (no engine patch). **In no
branch is a `patches/` engine change required** — the sharpest contrast with PiP, which needs a whole
chrome-layer PiP-window backend ported into the shell. If the probe returns (d), it is a dead end at
the *page* layer and we register it the way voice/PiP dead ends are registered.

Because there is a concrete, cheap, decisive spike and no engine blocker, a **Phase 11** is added to
TASKS.md (probe-first, in the shape of the touch-lock grab test): the spike decides whether the
feature exists at all before any code is written.

### Open questions (each needs the OLED Deck + the live site — cannot be closed on a workstation)
- Does `youtube.com/tv` expose a focus/hover preview surface **under our Cobalt UA** at all? (§1, §2b)
- If it does, is it (a) an animated `<img>`, or (b)/(c) a muted inline/MSE/DASH `<video>`? (§1)
- Is our **AV1 steering** suppressing an AV1-only preview? (§2a — config A/B)
- Is it **focus**-triggered (works today) or **pointer-hover**-gated (needs a synthesizer)? (§4)
- If it is a video, is it hw (`VaapiVideoDecoder`) or sw (`Dav1dVideoDecoder`) decoded, and what does
  it cost against the (still-unmeasured) P4 budget? (§5)

---
**Cross-reference:** `durable/pip.md` is the sibling investigation. **PiP and Preview are different
features with different verdicts** — PiP is a dead end (no window backend in the shell, no hostable
second surface under gamescope, no second stream from Leanback); Preview has *no* engine blocker and
its fate rests entirely on a live-app probe. Do not conflate them.
Version-scoped symbol/flag facts for this pin are appended to `milestones/m114.md` ("Focus/hover
preview — exact symbols at this pin").
