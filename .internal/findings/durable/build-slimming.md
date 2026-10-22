---
scope: durable
verified: 2026-07-13 (measurements at m138, pin a1d34a1a — re-verify numbers on every bump)
sources:
  - build-slimming spike (trial config cobalt/src/out/slim, since deleted)
  - cobalt/build/configs/{common,linux_common}.gn (upstream Cobalt's own slim configs)
---

# Build slimming — which Chromium features we can turn OFF (and which we must not)

Goal: faster `just build` and a smaller deployable, by disabling Chromium features the product
(YouTube Leanback kiosk on Steam Deck) never uses. The *method* and the *requirement→flag map* are
durable; every number and every "this flag exists / defaults to X" claim was measured at **m138**
(milestones/m138.md §"Build slimming spike").

**Applied:** the §3 set lives in `args/common.gn` (all presets), the dev/asan-only typecheck skip in
`args/{dev,asan}.gn`, and the web-test harness removal is `patches/0002-…-disable-web-tests.patch`.
Status of the gates is in §7.

## 1. The headline finding

Our platform config `chromium_linux-x64x11` is **essentially stock desktop Chromium**. It imports
only `chromium_linux.gn` (`target_os`, `chrome_pgo_phase=0`) — it does NOT import
`cobalt/build/configs/common.gn` or `linux_common.gn`, where upstream Cobalt disables WebGPU/Dawn,
SwiftShader, Vulkan, VR, TFLite, printing, remoting, the HSTS preload list, etc., each annotated with
measured size savings. `is_cobalt` is **false** in our build. Those files also assume the starboard
platform (ozone=starboard, no ffmpeg, no vaapi, no udev/glib/X11), which we cannot adopt wholesale,
so we cherry-pick the subset that survives our platform's needs.

## 2. Where the build actually goes (baseline, m138 `deck` preset)

`content_shell` = 42,019 ninja steps, 34,700 compile TUs, 1,412 MB unstripped. Top TU consumers:

| area | TUs | flag-removable on our platform? |
|---|---|---|
| third_party/blink | 9,012 (26%) | no (the engine) |
| components | 3,273 | marginal |
| content | 2,080 | no |
| ui | 1,749 | marginal (qt/gtk) |
| services | 1,579 | marginal (on_device_model is stuck, §5) |
| v8 | 1,510 | no |
| net | 1,197 | marginal |
| third_party/perfetto | 1,194 | **no** — base tracing, no off switch |
| third_party/webrtc | 1,160 | partially (codecs yes; core needs a patch, §5) |
| skia | 1,159 | partially (graphite/dawn bits) |
| third_party/xnnpack | 1,028 | **yes** (tflite/webnn) |
| media | 909 | partially |
| third_party/dawn | 724 | partially (§5: on_device_model pins the core) |
| third_party/swiftshader | 362 | **yes** |

~60% of the build (blink+v8+content+base+net+skia) is the engine itself and no GN arg removes it.
Flags shave the long tail; they cannot make this a small build.

## 3. The verified OFF set (gn-gen-clean at m138, built + smoke-gated as ONE unit)

The block below configures cleanly (`gn gen` passes all asserts) **as a whole** on top of the `deck`
preset (`qa`/official). Measured effect at m138:

- **Build:** 42,019 → 39,194 ninja steps (−6.7%); 34,700 → 32,017 compile TUs (−7.7%). Removed
  entirely: SwiftShader (−362 TUs), XNNPACK (−1,028), Vulkan validation layers + SPIR-V tools +
  vulkan-utility (−392), chromoting `remoting/` (−120), openh264 (−87), openscreen (−84), qt/gtk
  shims; partial cuts in dawn/angle/skia/gpu/media/webrtc/tflite.
- **Binary:** unstripped 1,412.2 → 1,352.2 MB; **stripped (what ships) 193.4 → 177.0 MB (−8.5%)**.
  `libvk_swiftshader.so` (29.4 MB) is not built → the deploy bundle sheds ~46 MB.
  (`libvulkan.so.1` is still produced; deploy.sh copies it conditionally — harmless.)
- With the §5.2 web-test patch (the shipped config): stripped **175.0 MB** (−9.5% vs baseline), TUs
  ~30.9k (−11%).
- `scripts/smoke.sh` passes on the shipped config: Leanback loads under the TV UA, AV1-steering
  baseline + assertions green, VP9/H.264 supported, clean welcome-screen screenshot — headless
  rendering survives SwiftShader removal.

```gn
# GPU: WebGPU off; ANGLE keeps ONLY the GL backend (runtime already forces --use-angle=gl).
use_dawn = false
skia_use_dawn = false
enable_swiftshader = false
angle_enable_swiftshader = false
enable_swiftshader_vulkan = false
enable_vulkan = false
angle_enable_vulkan = false
angle_use_vulkan_display = false
# Dawn CORE cannot leave the graph (§5, on_device_model) — strip every backend instead.
dawn_enable_vulkan = false
dawn_enable_desktop_gl = false
dawn_enable_opengles = false
dawn_use_swiftshader = false
dawn_enable_vulkan_validation_layers = false
dawn_enable_spirv_validation = false
# XR / ML
enable_vr = false
build_with_tflite_lib = false
build_tflite_with_xnnpack = false
webnn_use_tflite = false
# Printing / PDF / plugins — MUST move together, and glic is the linchpin (§4).
enable_glic = false
enable_pdf = false
enable_plugins = false
enable_printing = false
enable_print_preview = false
use_cups = false
enable_paint_preview = false
# Media long tail (WebRTC codecs, cast remoting, HLS/MPEG2-TS, capture)
enable_libaom = false
enable_hls_demuxer = false
enable_mse_mpeg2ts_stream_parser = false
media_use_openh264 = false
rtc_use_h264 = false
rtc_use_h265 = false
rtc_include_dav1d_in_internal_decoder_factory = false
rtc_use_pipewire = false
enable_media_remoting = false
enable_screen_capture = false
# Net / desktop-integration long tail
include_transport_security_state_preload_list = false
use_kerberos = false
enable_mdns = false
use_qt5 = false
use_qt6 = false
use_gtk = false
# Size, not speed: ~167 KB for <10 ms startup (upstream Cobalt ships this)
v8_enable_snapshot_compression = true
```

Product rationale for the riskier-looking entries:
- **WebGPU/Dawn, VR, WebNN/TFLite, screen capture, PDF, printing, chromoting**: Leanback uses none
  of these. Upstream Cobalt disables all of them for its certified builds.
- **rtc_use_h264/h265, openh264, libaom, rtc dav1d**: WebRTC *call* codecs (encode side, mostly).
  Voice search needs `getUserMedia` audio, which lives in mediastream + the audio service (pulse) —
  kept. libaom stays half-alive regardless, see §4.
- **HSTS preload list**: the kiosk goes straight to `https://www.youtube.com/tv`; a preload list of
  third-party domains protects first-contact `http://` navigations we never make. Upstream Cobalt:
  "we can trust clients to use https" (−1.3 MB rodata, and kills the slow
  `transport_security_state_generator` step).
- **qt/gtk**: chrome-layer desktop-integration shims; content_shell never initializes GtkUi. They
  leave the graph entirely.

## 4. Gotchas (asserts and traps)

- **`enable_pdf=false` alone FAILS gen**: `//chrome/browser/glic` (evaluated at gen time even though
  content_shell never builds it) asserts `enable_pdf`, and `enable_pdf=true` asserts
  `enable_plugins` and pulls `//printing`. The chain is **glic → pdf → plugins + printing**; set
  `enable_glic=false` to unlock the rest. Expect this chain to mutate on every bump.
- **`devtools_skip_typecheck=true` is REJECTED on official builds**
  (`assert(!devtools_skip_typecheck || !is_official_build)`). It is a **dev/asan-preset-only**
  compile-speed win (the devtools-frontend TypeScript pipeline is ~1,563 GN targets).
- **`enable_libaom=false` does NOT remove libaom**: `//media/gpu/vaapi` pulls
  `//third_party/libaom:libaomrc` (AV1 hardware-*encoder* rate control) unconditionally under
  `use_vaapi`. ~Half of libaom's TUs stay; removing it is patch-level (we never encode).
- **`use_dawn=false` does NOT remove Dawn**: `//services/on_device_model` (pulled unconditionally by
  `//content/utility`) links `dawn/native`. No off-switch arg exists at m138
  (`enable_ml_internal=false` already). Stripping every Dawn *backend* (block above) is the
  workaround — it kills SwiftShader, Vulkan validation layers, and SPIR-V tools transitively.
- **`ninja -t commands` counts are inflated ~2×** if you grep bare `.o` paths (each object appears
  in its compile AND its link/archive line). Count `-o obj/...` occurrences for true TU numbers.
- **A failed `gen.sh` leaves `out/<preset>/args.gn` BARE**: gn.py regenerates args.gn first, and
  gen.sh appends the `# --- deckback extra args` block only after `gn gen` succeeds. If gn gen fails
  in between, a later `build.sh` builds a STOCK config (no proprietary_codecs, no slim set) and
  reports success. Smoke catches it (H.264 asserts fail); to check directly:
  `grep -c 'deckback extra args' out/<preset>/args.gn` must be 1.

## 5. Blocked / not removable by args at m138 (patch-level opportunities, ranked)

1. **WebRTC core (~1,125 TUs remain) + blink peerconnection**: upstream Cobalt has exactly our
   switch — `use_webrtc_peer_connection = !is_cobalt` in `third_party/blink/public/public_features.gni`
   (drops RTCPeerConnection, KEEPS getUserMedia/mic). Setting it false on our platform **fails gen**:
   `//third_party/blink/renderer/bindings/BUILD.gn` references `cobalt_bindings_exclude_patterns`,
   defined only when `is_cobalt=true` (an upstream bug). A one-line guard in `patches/` would unlock
   the biggest remaining flag-shaped cut.
2. **content_shell's web-test harness (~1,134 TUs)** — ★ DONE as
   `patches/0002-…-disable-web-tests.patch`: `content/shell/BUILD.gn` hardcodes
   `support_web_tests = !is_android` (a local variable, NOT a gn arg), so `content_shell_lib` always
   links `//content/test:test_support`, several `//components/*:test_support` targets, and the
   web_test browser/renderer classes — dead unless launched with `--run-web-tests`. The patch flips
   it to `false` (~3.3% of the build).
3. **devtools-frontend (~1,563 GN targets, TS actions + grd pack)**: content_shell embeds the
   frontend resources. CDP itself (launcher, cdp.py, `just debug` via chrome://inspect, which uses
   the *workstation's* frontend) does not need them. Patch-level: stub
   `devtools_frontend_resources` out of `//content/shell`. Do NOT attempt until someone confirms
   nothing loads `http://localhost:9222/devtools/inspector.html` directly from the Deck binary.
4. **perfetto (1,194 TUs)**: welded into base tracing; no supported off switch. Leave it.
5. **on_device_model / on_device_translation services**: unconditional in content; backends already
   neutered via the Dawn block. Leave the husks.

## 6. Deliberately KEPT ON — the requirement map (check before ever flipping these)

| flag | why it stays |
|---|---|
| `media_use_libvpx` | **software VP9 fallback** when VA-API fails/regresses — our safety net |
| `enable_dav1d_decoder`, `enable_av1_decoder` | (a) smoke's AV1-steering assertion runs a BASELINE that *requires* `isTypeSupported(av01)==true` pre-steering (an engine that can't do AV1 false-passes every steering check); (b) turning off only dav1d leaves `enable_av1_decoder=true` → the engine *claims* AV1 it cannot decode. Removing AV1 means redesigning the smoke gate first |
| `enable_hevc_parser_and_hw_decoder` | YT fallback formats include HEVC (doc hard constraint alongside H.264/AAC) |
| `proprietary_codecs`, `ffmpeg_branding="Chrome"`, `media_use_ffmpeg` | doc hard constraint — H.264/AAC break without them |
| `use_vaapi` | P4 hardware decode |
| `enable_widevine` (+ `enable_library_cdms` default) | P7; the carried CDM-registration patch depends on it |
| `use_udev`, `use_dbus`, `use_glib`, `use_pulseaudio`, `use_alsa` | audio is hardware-verified; udev feeds gpu/device monitoring; unknown coupling — near-zero TU win, real regression risk |
| `use_xkbcommon`, ozone x11 | we launch with `--ozone-platform=x11` under gamescope/XWayland |
| `angle_enable_gl` | the shipped `--use-angle=gl` path |
| `icu_use_data_file` | flipping it embeds ICU in the binary; zero net size win, packaging churn |
| `v8_enable_maglev`, no `optimize_for_size` | JS/runtime perf for Leanback UI snappiness; the Deck's Zen 2 is not a TV SoC. Both are *size* levers upstream Cobalt pulls for weaker targets — revisit only if size becomes a gate |

## 7. Gate status

- **Passed (workstation):** `just gen` for all three presets, `just build deck`, `just smoke deck`
  on the shipped config.
- **Passed (on-Deck, OLED):** the DUAL pixel gate on a build carrying the slim args — L2
  `test_hardware_decode_is_vaapi` + `test_video_is_not_corrupt`, 2026-10-21. This mattered because
  the ANGLE build config changed (Vulkan backend deleted). `just power` on that build read 6.39 W
  (PASS; not comparable to the earlier baseline, see `hardware.md`).
- **Open (on-Deck):** `just cert-deck` (Widevine L3 conformance must still be 45/46) and a mic /
  `getUserMedia` sanity pass (WebRTC was trimmed).
- If smoke dies in GPU init under Xvfb after a bump, suspect the SwiftShader/Vulkan removals first.
- On every Cobalt bump, re-run the *method* (§8), not this list: flag names, defaults, and the
  glic-style assert chains drift.

## 8. The method (reusable verbatim on any bump)

All read-only, minutes each, no compiles needed until the final gate:

```sh
# 1. Ground truth of what's on/off (never trust docs or this file):
just gen deck   # if out/deck is stale
in-container: gn args out/deck --list --json > /tmp/args.json
# 2. Is a feature actually in OUR target's graph? (many chrome-layer flags are no-ops for content_shell)
in-container: gn desc out/deck //content/shell:content_shell deps --all | grep -c <area>
# 3. WHY is it in the graph (finds the real gate, or that there is none):
in-container: gn path out/deck //content/shell:content_shell <target>
# 4. Trial config: copy out/deck/args.gn -> out/slim/args.gn, append candidates, then
in-container: gn gen out/slim          # every assert failure here is a §4-style finding — record it
# 5. Quantify without building:
in-container: third_party/ninja/ninja -C out/<dir> -t commands content_shell | wc -l          # steps
             ... | grep -oE '\-o obj/[a-zA-Z0-9_/.-]+\.o' | sed 's#-o obj/##;s#/.*##' | sort | uniq -c  # TUs/area
# 6. Only then build + smoke the trial dir (smoke.sh takes any out/<name> as its preset arg).
```

Also re-read `cobalt/build/configs/{common,linux_common}.gn` on each bump — upstream keeps adding
measured slimmings there (our best candidate feed) — and re-check whether upstream fixed the
`use_webrtc_peer_connection` / `is_cobalt=false` gen bug (§5.1).
