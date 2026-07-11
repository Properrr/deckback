---
scope: durable
verified: 2026-07-09
method: read the pinned Cobalt tree (cobalt/src @ DEPS.pin a9181df8); no on-Deck run
sources:
  - cobalt/src (content/, third_party/blink/, chrome/, media/gpu/)
  - .internal/findings/milestones/m114.md (§"Picture-in-Picture" — the exact symbols/lines)
  - .internal/findings/durable/hardware.md (VA-API facts, AV1 dispute)
---

# Picture-in-Picture (PiP) — feasibility

**Verdict: NOT deliverable on this engine as a real browser PiP window, and not deliverable in a
degraded in-page form either — the first is blocked in the engine, the second is blocked by
Leanback.** The Blink PiP JavaScript API is fully compiled in and *feature-detects as available*
(`document.pictureInPictureEnabled === true`, `video.requestPictureInPicture` is a function), but the
content-embedder window backend is stubbed out in the Cobalt/content shell exactly the way Widevine's
`AddContentDecryptionModules` is — so calling `requestPictureInPicture()` **rejects at runtime** with
`NotSupportedError`. This is a dead-button trap: the API looks present and does nothing. It goes on
the same shelf as `voice_search`/`kSbKeyMicrophone` and the Widevine registration gap. **Do not ship
a PiP button.**

Everything below is *verified by reading the pinned tree*, not on hardware. Nothing here was run on a
Deck; the on-Deck items are named explicitly as open questions with the test that would close them.

> **Sibling investigation — do not conflate.** `durable/preview.md` covers the TV **focus/hover
> video preview** (a muted inline preview clip / animated thumbnail on the focused tile). It is a
> **different feature with a different verdict**: preview is ordinary in-page content with **no engine
> blocker** (no PiP-window backend needed, muted autoplay enabled, no per-page player cap), and its
> fate rests on a live-app probe, not on the shell embedder gap that kills PiP below. The PiP verdict
> does **not** transfer to preview.

## 1. The engine has the API surface but not the backend (the trap)

Trace, decisive link last:

1. **Binding exists, unconditionally exposed.** `video.requestPictureInPicture()` is a plain partial
   interface on `HTMLVideoElement` with no `RuntimeEnabled` gate
   (`third_party/blink/renderer/modules/picture_in_picture/html_video_element_picture_in_picture.idl`).
   `document.pictureInPictureEnabled` is backed by the Blink setting `pictureInPictureEnabled`, whose
   default is **`initial: true`** (`third_party/blink/renderer/core/frame/settings.json5:212`) and
   whose `WebPreferences` default is also `true`
   (`third_party/blink/common/web_preferences/web_preferences.cc:208`). No shell sets it false. **So
   feature detection lies: it reports available.**

2. **Blink dispatches the request into content.**
   `PictureInPictureControllerImpl::EnterPictureInPicture`
   (`third_party/blink/renderer/modules/document_picture_in_picture/picture_in_picture_controller_impl.cc:127`)
   passes `IsDocumentAllowed`/`IsElementAllowed` (the setting is true, so `Status::kEnabled`) and
   calls `picture_in_picture_service_->StartSession(...)`.

3. **Content asks the embedder — and the embedder says no.**
   `VideoPictureInPictureWindowControllerImpl::StartSession`
   (`content/browser/picture_in_picture/video_picture_in_picture_window_controller_impl.cc:232`) does
   `auto result = GetWebContentsImpl()->EnterPictureInPicture();` and **returns early if it is not
   `kSuccess`** (lines 236-237). `WebContentsImpl::EnterPictureInPicture` forwards to the
   `WebContentsDelegate` (`content/browser/web_contents/web_contents_impl.cc:8956`). The delegate is
   the shell, and **the shell hard-codes refusal**:
   - `cobalt/shell/browser/shell.cc:1000` → `return PictureInPictureResult::kNotSupported;`
   - `content/shell/browser/shell.cc:716-721` → `kNotSupported` unless `--run-web-tests` is present.

4. **Even past that gate there is no window to create.** The window is built by
   `GetContentClient()->browser()->CreateWindowForVideoPictureInPicture(this)`
   (same controller, line 251). The base `ContentBrowserClient` default **returns `nullptr`**
   (`content/public/browser/content_browser_client.cc:1137-1141`). Neither
   `cobalt/browser/cobalt_content_browser_client.*` nor `cobalt/shell/browser/shell_content_browser_client.*`
   overrides it — only `chrome/browser/chrome_content_browser_client.cc:6589` does, and it delegates to
   `content::VideoOverlayWindow::Create`, which is **defined only under `chrome/`**
   (`chrome/browser/ui/views/overlay/video_overlay_window_views.cc:307` and the Android variant).
   `grep` for `VideoOverlayWindow::Create` across `content/`, `cobalt/`, `components/` finds **nothing**
   — the whole `views::Widget`-based PiP window + `PictureInPictureWindowManager`
   (`chrome/browser/picture_in_picture/`) is chrome-layer code the shell does not build.

5. **The rejection surfaces to JS.** With no session, `OnEnteredPictureInPicture` gets a null
   `session_remote` and rejects the promise with
   `DOMExceptionCode::kNotSupportedError, "Picture-in-Picture is not available."`
   (`.../picture_in_picture_controller_impl.cc:201-212`).

**Same shape as two already-registered traps:** Widevine (`enable_widevine` compiles support but no
shell overrides `AddContentDecryptionModules`, so `CdmRegistry` is empty — m114.md) and voice
(`kSbKeyMicrophone` exists but Chrobalt has zero Starboard symbols to route it — TASKS P5). A binding
that resolves to an unimplemented embedder delegate is not a feature.

**What would make the browser-PiP path exist:** a `patches/` quilt entry that (a) makes the shell
`WebContentsDelegate::EnterPictureInPicture` return `kSuccess`, and (b) provides a real
`CreateWindowForVideoPictureInPicture` returning a working `VideoOverlayWindow`. (b) is not a small
override — it is the `chrome/browser/ui/views/overlay/` Views window stack (~a `views::Widget` +
Aura window-tree-host), which the shell does not currently link. This is far larger than the
one-function Widevine patch and pulls chrome UI toolkit surface into the shell. Before any of that is
worth costing, Q2 (below) has to say a second window is even usable — and it does not.

## 2. There is no place to put a PiP window (gamescope / Game Mode)

Even granting the engine patch in §1, a real PiP window is a **second top-level Xwayland surface**.
We run one fullscreen Xwayland surface at 1280×800, composited by gamescope in Game Mode
(`gamescope --xwayland-count 2 -w 1280 -h 800 ... -O *,eDP-1`, m114.md "Test hardware"). Game Mode
gamescope presents a **single focused surface fullscreen**; it is not a window manager the user can
tile, move, or place a floating child window in. A second top-level from the same app is not a
design gamescope Game Mode supports for us, and the user has no WM to interact with it.

- **This is reasoned from the documented gamescope invocation + Game Mode behavior, not measured.**
  Open question (would need on-Deck): does gamescope Game Mode show a second top-level at all, and if
  so where. But it is moot, because §1 means no second top-level is ever created regardless.

**Conclusion the evidence supports:** the only design that could live on this device is an
**in-page overlay** — a second `<video>` composited by the page *inside our one surface* — never a
real browser PiP window. That design's viability collapses on Q4 (Leanback won't give us a second
stream), so it is not a rescue.

## 3. Multi-decode is NOT the binding constraint (but the numbers are unmeasured)

The user's hypothesis was that a 240p secondary is cheap. Whether that is true is **not** what
decides PiP here — §1/§2/§4 kill it before cost matters. For completeness and honesty:

- **Chromium does not cap concurrent decoders at 2.** `VaapiVideoDecoder::kMaxNumOfInstances = 16`
  (`media/gpu/vaapi/vaapi_video_decoder.h:207`); the guard
  (`media/gpu/vaapi/vaapi_video_decoder.cc:113-119`, feature `kLimitConcurrentDecoderInstances`,
  `FEATURE_ENABLED_BY_DEFAULT` at `media/base/media_switches.cc:1115`) only refuses the **17th**.
  Two concurrent VA-API decode sessions are not blocked by any Chromium software cap.
- **Whether the Van Gogh/Sephiroth VCN + radeonsi can actually run two simultaneous `VAEntrypointVLD`
  contexts is a hardware/driver fact we cannot read out of the tree.** libva enumerating a profile
  says nothing about concurrent-context capacity (same lesson as the AV1 dispute: enumeration ≠
  observed behavior — hardware.md, m114.md §"Media stack"). **Unmeasured.** The test that would settle
  it: on-Deck, drive two `<video>` elements decoding simultaneously and read `kIsPlatformVideoDecoder`
  / `kVideoDecoderName` on **both** players via the CDP `Media` domain (TEST-PLAN §5); a fallback to
  `FFmpegVideoDecoder`/`Dav1dVideoDecoder` on the second is the signal the driver serialized or refused.
- **The "240p on the CPU is cheap" hypothesis: I could not find real numbers in-tree, so I will not
  invent any.** There are no dav1d/libvpx 240p-decode wattage figures in this repo, and the one
  playback power figure we have (≤~8–9 W 1080p VP9, **OLED-measured, hw decode** — hardware.md) is a
  different codec, resolution, and decode path. The P4 ≤9 W budget itself has **never been measured**
  (`power.sh` still asks a human to start a video; TEST-PLAN §E). So there is no baseline to add a
  secondary decode *to*. The measurement that would settle the cost question: on-Deck, play the
  primary at the normal 720p/1080p VP9 while decoding a 240p secondary in software, and sample
  `power_now` before/after (closed-loop, the T5 work) — but this only becomes worth running if Q4
  ever yields a second stream, which it does not.

## 4. Leanback will not hand us a second stream (the crux)

The UI is `youtube.com/tv` — a server-side single-player SPA we do not control and cannot modify
(the whole project rests on Google's tolerance of our TV UA; we inject only via CDP overrides, never
into their app logic). PiP as a product needs a *second, independently controlled* video. Realistic
sources and why each fails here:

- **A second `<video>` / second MSE `MediaSource` inside Leanback** — Leanback is a single-player
  app; it does not expose a second player element for us to drive, and we cannot make it create one.
  Our `player.cpp` already assumes exactly one: `document.querySelector('video')` (singular) is the
  entire play-state model (`launcher/src/player.cpp:34-51`). We would be composing over an app that
  has one video by design.
- **An embedded iframe player** — not something Leanback offers, and not something we can graft in.
- **A separate origin/URL we load ourselves (`youtube.com/embed`, `/watch`, a second `/tv`)** — this
  is no longer "PiP within the app". It means running a **second page**, and because Leanback tears
  its own CDP target down on navigation and holds one profile/session, a genuinely independent second
  stream realistically means a **second engine instance** (a second `content_shell`). Price: another
  full renderer + GPU process. We already track RSS and the plan only *considers* `--process-per-site`
  as a memory lever for the single instance (m114.md "Platform-optimization research";
  `content_switches.cc:621`); a whole second engine is the opposite direction. On a 16 GB shared-memory
  handheld running under gamescope, that is a heavy, fragile answer to a feature with a weak user story
  (§5) — and it still has nowhere to draw the second window (§2).

**So even the degraded in-page-overlay design has no content to put in the overlay** without a second
engine. That is the crux: the block is architectural, upstream of any decode-cost question.

## 5. What would PiP even be *for* here — the two user stories, sharply distinguished

- **"Keep watching over another Steam game."** This is a gamescope/Steam-compositor feature (a video
  overlay above a different running app). We are one fullscreen kiosk with no compositor control; we
  cannot deliver it, full stop. Not our layer.
- **"Keep watching a shrunk video while browsing the YouTube rails."** This is *in-app* behavior —
  and Leanback already owns it server-side (its own mini/continue-watching behavior), rendered inside
  the single surface we hand it. If YouTube's TV app shrinks the player while you browse, that happens
  today with **zero** work from us and is not "PiP" in the browser sense. There is nothing for us to
  build and nothing we *can* build to change it.

The user proposed 240p because it is cheap. **Cost is not the binding constraint.** The binding
constraints are: (1) the engine has no PiP window backend (§1); (2) gamescope Game Mode has nowhere
to host a second window (§2); (3) Leanback won't yield a second stream and we can't inject one (§4).
A cheaper secondary stream does not touch any of the three.

## 6. Verdict and what would have to change

**Not viable at pin `a9181df8` (m114). No `patches/` entry is worth writing today**, because a
shell-side `EnterPictureInPicture`/`CreateWindowForVideoPictureInPicture` patch (§1) would only make
`requestPictureInPicture()` *resolve*, then immediately hit §2 (no window host) and §4 (no second
stream). Nothing is added to TASKS.md.

For this to become viable, **all three** would have to change together:

1. **Engine:** a `patches/` entry giving the shell a real `VideoOverlayWindow` backend — i.e. porting
   or stubbing the chrome-layer `views::Widget` PiP window into the shell, plus flipping the delegate
   to `kSuccess`. Large; pulls chrome UI toolkit surface into the shell; rebases every Cobalt bump.
2. **Compositor:** evidence (on-Deck) that gamescope Game Mode will host a second top-level surface at
   all — or a redesign to an in-page overlay that needs no second window.
3. **Content:** a second, independently controllable stream. Realistically a second engine instance
   loading a separate YouTube surface (the memory/complexity cost in §4) — Leanback itself will not
   provide one.

Until an upstream Cobalt bump ships a shell that implements the PiP embedder delegate **and** the
project accepts a second engine instance **and** an on-Deck test shows a second surface is hostable
under gamescope, PiP stays a dead end. Registering the dead end is the deliverable — per the project's
stated preference for naming a dead end over shipping a dead button (voice_search / `kSbKeyMicrophone`
precedent, TASKS P5).

### Open questions (each needs the OLED unit — cannot be closed on a workstation)
- Does gamescope Game Mode host a second top-level Xwayland surface, and where? (§2)
- Can Van Gogh/Sephiroth VCN + radeonsi run two simultaneous `VAEntrypointVLD` contexts? Two-player
  CDP `Media`-domain probe. (§3) — moot for PiP, but a genuine hardware unknown worth its own line.
- Baseline P4 power has never been measured; a secondary-decode cost delta has no baseline to add to. (§3)
