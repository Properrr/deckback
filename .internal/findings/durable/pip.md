---
scope: durable
verified: 2026-07-09
method: read the pinned Cobalt tree (first at the M114 pin a9181df8; the engine-side links in §1/§3 re-checked at the M138 pin a1d34a1a, 2026-10-21); no on-Deck run
sources:
  - cobalt/src (content/, third_party/blink/, chrome/, media/gpu/)
  - .internal/findings/durable/hardware.md (VA-API facts, AV1 dispute)
---

# Picture-in-Picture (PiP) — feasibility

**Verdict: NOT deliverable on this engine as a real browser PiP window, and not deliverable in a
degraded in-page form either — the first is blocked in the engine, the second is blocked by
Leanback.** The Blink PiP JavaScript API is compiled in and *feature-detects as available*
(`document.pictureInPictureEnabled === true`, `video.requestPictureInPicture` is a function), but the
embedder window backend is stubbed out in the Cobalt/content shell the same way Widevine's
`AddContentDecryptionModules` was — so `requestPictureInPicture()` **rejects at runtime** with
`NotSupportedError`. A dead-button trap: the API looks present and does nothing. **Do not ship a PiP
button.**

Everything below is *verified by reading the tree*, not on hardware. The on-Deck items are named as
open questions with the test that would close them.

> **Sibling investigation — do not conflate.** `durable/preview.md` covers the TV **focus/hover
> video preview** (a muted inline clip / animated thumbnail on the focused tile). It is ordinary
> in-page content with **no engine blocker**, and its fate rests on a live-app probe. The PiP verdict
> does **not** transfer to preview.

## 1. The engine has the API surface but not the backend (the trap)

Trace, decisive link last (symbols re-checked at M138):

1. **Binding exists, unconditionally exposed.** `video.requestPictureInPicture()` is a plain partial
   interface on `HTMLVideoElement` with no `RuntimeEnabled` gate
   (`third_party/blink/renderer/modules/picture_in_picture/html_video_element_picture_in_picture.idl`).
   `document.pictureInPictureEnabled` is backed by the Blink setting `pictureInPictureEnabled`,
   default **`initial: true`** (`third_party/blink/renderer/core/frame/settings.json5`), and no shell
   sets it false. **Feature detection reports available.**
2. **Blink dispatches the request into content.**
   `PictureInPictureControllerImpl::EnterPictureInPicture` passes `IsDocumentAllowed`/
   `IsElementAllowed` and calls `picture_in_picture_service_->StartSession(...)`.
3. **Content asks the embedder — and the embedder says no.**
   `VideoPictureInPictureWindowControllerImpl::StartSession` returns early unless
   `WebContentsImpl::EnterPictureInPicture()` (forwarded to the `WebContentsDelegate`, i.e. the shell)
   returns `kSuccess`. **The shell hard-codes refusal:**
   - `cobalt/shell/browser/shell.cc` → `return PictureInPictureResult::kNotSupported;`
   - `content/shell/browser/shell.cc` → `kNotSupported` unless `--run-web-tests` is present.
4. **Even past that gate there is no window to create.** The window comes from
   `ContentBrowserClient::CreateWindowForVideoPictureInPicture`, whose base default **returns
   `nullptr`** (`content/public/browser/content_browser_client.cc`). Neither the Cobalt nor the shell
   content browser client overrides it — only `chrome/browser/chrome_content_browser_client.cc` does,
   delegating to `content::VideoOverlayWindow::Create`, which is **defined only under `chrome/`**
   (`chrome/browser/ui/views/overlay/`). Nothing in `content/`, `cobalt/` or `components/` defines it:
   the `views::Widget` PiP window + `PictureInPictureWindowManager` are chrome-layer code the shell
   does not build.
5. **The rejection surfaces to JS.** With no session the promise rejects with
   `DOMExceptionCode::kNotSupportedError, "Picture-in-Picture is not available."`
   (`picture_in_picture_controller_impl.cc`).

**Same shape as the Widevine trap** (`enable_widevine` compiled support but no shell override of
`AddContentDecryptionModules`, so `CdmRegistry` was empty — hence `patches/0001`): a binding that
resolves to an unimplemented embedder delegate is not a feature.

**What would make the browser-PiP path exist:** a `patches/` entry that (a) makes the shell
`WebContentsDelegate::EnterPictureInPicture` return `kSuccess`, and (b) provides a real
`CreateWindowForVideoPictureInPicture` returning a working `VideoOverlayWindow`. (b) is the
`chrome/browser/ui/views/overlay/` Views window stack (a `views::Widget` + Aura window-tree-host),
which the shell does not link — far larger than the Widevine patch, and it pulls chrome UI toolkit
surface into the shell. §2 says a second window would not be usable anyway.

## 2. There is no place to put a PiP window (gamescope / Game Mode)

A real PiP window is a **second top-level Xwayland surface**. We run one fullscreen Xwayland surface
at 1280×800, composited by gamescope in Game Mode (`gamescope --xwayland-count 2 -w 1280 -h 800 ...
-O *,eDP-1`). Game Mode gamescope presents a **single focused surface fullscreen**; it is not a window
manager the user can tile, move, or place a floating child window in.

- **Reasoned from the documented gamescope invocation + Game Mode behaviour, not measured.** Open
  question: does gamescope Game Mode show a second top-level at all, and where? Moot while §1 holds.

The only design that could live on this device is an **in-page overlay** — a second `<video>`
composited by the page inside our one surface. That design collapses on §4 (no second stream).

## 3. Multi-decode is NOT the binding constraint (and the numbers are unmeasured)

§1/§2/§4 kill PiP before decode cost matters. For completeness:

- **Chromium does not cap concurrent hardware decoders at 2.** At M138 the cap is
  `VaapiWrapper::GetMaxNumDecoderInstances()` (`media/gpu/vaapi/vaapi_wrapper.cc`): **16** by default
  (10 only on AMD Stoney Ridge), enforced only while `kLimitConcurrentDecoderInstances` is enabled
  (`FEATURE_ENABLED_BY_DEFAULT`, `media/base/media_switches.cc`). Two concurrent VA-API sessions are
  not blocked by any Chromium software cap.
- **Whether Van Gogh/Sephiroth VCN + radeonsi can run two simultaneous `VAEntrypointVLD` contexts is
  a hardware/driver fact the tree cannot answer.** libva enumerating a profile says nothing about
  concurrent-context capacity (same lesson as the AV1 dispute — `hardware.md`). **Unmeasured.** The
  test: on-Deck, decode two `<video>` elements simultaneously and read `kIsPlatformVideoDecoder` /
  `kVideoDecoderName` on **both** players via the CDP `Media` domain (TEST-PLAN §5); a fallback to
  `FFmpegVideoDecoder`/`Dav1dVideoDecoder` on the second means the driver serialized or refused.
- **"A 240p secondary on the CPU is cheap" has no numbers behind it.** No dav1d/libvpx 240p decode
  wattage exists in this repo. The measured P4 baseline is single-stream VP9 under hardware decode
  (≈5.6–5.9 W, OLED — `hardware.md`); the cost of adding a software-decoded secondary is unmeasured.
  The test: play the primary normally while decoding a 240p secondary in software and sample
  `power_now` before/after (`just power`) — worth running only if §4 ever yields a second stream.

## 4. Leanback will not hand us a second stream (the crux)

`youtube.com/tv` is a server-side single-player SPA we do not control (we inject only via CDP, never
into its app logic). PiP needs a *second, independently controlled* video:

- **A second `<video>` / MSE `MediaSource` inside Leanback** — Leanback is a single-player app and
  exposes no second player element for us to drive. Our own scripts assume exactly one:
  `document.querySelector('video')` (singular) is the whole play-state model (`player_state.js` and
  the other player scripts).
- **An embedded iframe player** — not something Leanback offers or we can graft in.
- **A separate URL we load ourselves (`youtube.com/embed`, `/watch`, a second `/tv`)** — a second
  page; because Leanback tears its CDP target down on navigation and holds one profile/session, an
  independent second stream realistically means a **second engine instance** (another full renderer
  + GPU process) on a 16 GB shared-memory handheld — and it still has nowhere to draw (§2).

So even the in-page-overlay design has no content to put in the overlay without a second engine.
The block is architectural, upstream of any decode-cost question.

## 5. What would PiP even be *for* here

- **"Keep watching over another Steam game."** A gamescope/Steam-compositor feature (a video overlay
  above a different app). We are one fullscreen kiosk with no compositor control. Not our layer.
- **"Keep watching a shrunk video while browsing the rails."** In-app behaviour Leanback owns
  server-side (its own mini/continue-watching behaviour), rendered inside our surface with zero work
  from us. Nothing for us to build.

**Cost is not the binding constraint.** The binding constraints are: (1) no PiP window backend in the
engine (§1); (2) nowhere to host a second window under gamescope Game Mode (§2); (3) no second stream
from Leanback (§4). A cheaper secondary stream touches none of them.

## 6. Verdict and what would have to change

**Not viable. No `patches/` entry is worth writing**: a shell-side `EnterPictureInPicture` /
`CreateWindowForVideoPictureInPicture` patch (§1) would only make `requestPictureInPicture()`
*resolve*, then hit §2 (no window host) and §4 (no second stream).

For this to become viable, **all three** must change together:

1. **Engine:** a `patches/` entry giving the shell a real `VideoOverlayWindow` backend plus flipping
   the delegate to `kSuccess`. Large; pulls chrome UI toolkit surface into the shell; rebases every
   Cobalt bump.
2. **Compositor:** on-Deck evidence that gamescope Game Mode will host a second top-level surface —
   or a redesign to an in-page overlay that needs no second window.
3. **Content:** a second, independently controllable stream — realistically a second engine instance
   (§4); Leanback will not provide one.

Registering the dead end is the deliverable: name a dead end rather than ship a dead button.

### Open questions (each needs the OLED unit)
- Does gamescope Game Mode host a second top-level Xwayland surface, and where? (§2)
- Can Van Gogh/Sephiroth VCN + radeonsi run two simultaneous `VAEntrypointVLD` contexts? Two-player
  CDP `Media`-domain probe. (§3) — moot for PiP, but a genuine hardware unknown.
- What does a software-decoded secondary stream cost against the measured P4 baseline? (§3)
