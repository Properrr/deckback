# Legal notes

Working notes, not legal advice. Review before Flathub submission (doc §12, risk R5/R6).

## License & disclaimer

- Deckback's own code and docs are licensed under **Apache License 2.0** (see [`../LICENSE`](../LICENSE)
  and [`../NOTICE`](../NOTICE)). Copyright 2026 The Deckback Authors.
- Apache-2.0's §7 (Disclaimer of Warranty) and §8 (Limitation of Liability) provide the "as is",
  no-warranty, no-liability posture. The README carries a plain-language **Disclaimer** restating
  that Deckback is unofficial, unaffiliated with Google/YouTube, and used at the user's own risk.
- Apache-2.0 §6 (Trademarks) grants no rights to any marks — consistent with the trademark rules
  below; names are used only nominatively.
- The Chromium/Cobalt engine is fetched at build time (not vendored here) and stays under its own
  BSD-style license; any user-supplied Widevine CDM stays under Google's terms. Apache-2.0 covers
  only this project's original contributions.

## Trademark (R6)

- No "YouTube" / "YT" in the app id, store name, or binary name. App id: `io.github.properrr.deckback`.
- No red play-button or YouTube-logo derivatives in the icon or grid art — original art only.
- Description must state: *"unofficial client for YouTube's TV interface, built on the open-source
  Cobalt engine."*

## Widevine CDM (R5)

- The Widevine CDM (`libwidevinecdm.so`) is **never redistributed or bundled** by this project.
- **As implemented** (`launcher/src/cdm_fetcher.cpp`): the launcher only *detects* an already-installed
  CDM. It never auto-downloads. A user may opt in by setting `cdm_url` (a source they trust) +
  `cdm_sha256` in `app.json`; the launcher then downloads it, **verifies the SHA-256**, and installs it
  into the user's profile dir. This is deliberately more conservative than auto-fetching.
- **For Flathub** (Phase 8/10): wire the standard Chromium **extra-data** pattern (Flathub fetches the
  component at install/first-run from Google's endpoint, hash-pinned in the manifest) rather than
  shipping a URL in config. Decided in Phase 8.
- L3 software CDM → DRM'd content plays but is resolution-capped by YouTube policy. Documented as
  best-effort; free YouTube is the product.

## Codecs

- `proprietary_codecs=true` / `ffmpeg_branding="Chrome"` ship H.264/AAC/HEVC — fine for GitHub
  releases. For Flathub, align with how Flathub's own Chromium handles codecs (runtime
  ffmpeg-full / openh264 extension) — decided in Phase 8.

## Conformance tests ≠ certification

The project's test suite can run Google's open-source `youtube/js_mse_eme` harness (MSE/EME/codec
conformance) against our build. That harness is published under its own licence and is **not
redistributed by this project** — it is fetched at a pinned commit, and its media test vectors are
fetched from Google's public bucket, hash-verified, and cached locally. Neither is vendored into a
release artifact.

Passing it **does not make this app YouTube-certified** and confers no status, endorsement, or
affiliation. YouTube device certification is a partner programme with its own requirements and
review. We use the harness purely as a regression net. Nothing in this project's user-facing
material may describe Deckback as "certified", "YouTube-approved", or similar.

The automated conformance run is **ClearKey-only** and never downloads the Widevine CDM, so no CI
job acquires or redistributes Google's CDM binary (see above).

## Posture

Like VacuumTube and Kodi YouTube plugins, the app depends on Google's tolerance of a TV-class UA.
State this openly in the README (doc risk R1).
