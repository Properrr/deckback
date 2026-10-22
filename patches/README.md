# patches/

A rebaseable quilt-style series of patches applied on top of the pinned Cobalt commit
(`DEPS.pin`). **Never edit `cobalt/` in place and leave it** — the checkout is gitignored and
`just sync` will re-apply this series over a clean tree.

**The series holds exactly two patches, and that is the design working.** Gamepad input, the
`MediaCapabilities` AV1 steering, and the mic auto-grant live in `launcher/` over the DevTools
protocol instead. Prefer the launcher: a patch is a recurring rebase cost, a CDP call is not.

## `0001-…-widevine-cdm-registration`

Unavoidable, because the engine offers no other seam. `content_shell` does not override
`ContentClient::AddContentDecryptionModules`, so the `CdmRegistry` is empty and every
`com.widevine.alpha` request is rejected no matter what `enable_widevine` says. There is no CDP call,
no command-line switch, and no runtime hook that fills it.

Two facts from the tree that are easy to get wrong:

- **The capability must be concrete at registration.** Upstream lazily initialises the software-secure
  capability from the CDM's `manifest.json`. On Linux, `CdmRegistryImpl::LazyInitializeCapability()`
  falls into the `#else` branch and hands back an empty optional — the comment right there reads
  *"kSoftwareSecure should have been determined from the manifest"*. Register with an empty optional
  and the CDM loads, the registry accepts it, and `requestMediaKeySystemAccess` **still rejects**,
  with nothing in the log to explain why.
- **`enable_library_cdms` is already `true`** (it is `toolkit_views && !is_castos`). Only
  `enable_widevine = true` in `args/deck.gn` is ours.

The CDM path arrives on `--widevine-cdm-path`, which the launcher passes from
`CdmFetcher::installed_path()` and **only when a CDM is actually installed**. We never bundle or
redistribute Google's CDM (`docs/legal.md`); unencrypted YouTube is unaffected by its absence.

Status: the patch compiles and links under the `deck` preset and regresses neither `just smoke` nor
`just cert`. With no CDM installed it registers nothing, and `com.widevine.alpha` rejects with
`NotSupportedError`. **Not verified:** that a real CDM loads, that a licence exchange succeeds, or
that any DRM video plays. That needs a CDM on a Deck, and
`tests/deck/test_media.py::test_widevine_state` is the test that will say so.

## `0002-…-disable-web-tests`

Sets `support_web_tests = false` in `content/shell/BUILD.gn` (with the matching
`CONTENT_SHELL_DISABLE_WEB_TESTS` guard in `shell_main_delegate`). `content_shell` is upstream's
web-test shell and otherwise links ~1,134 translation units of test harness into every deployable
binary; the kiosk never runs `--run-web-tests`. Build-only: no runtime behaviour changes. See
`.internal/findings/durable/build-slimming.md`.

## Workflow

```sh
just patch-new <name>     # after committing your change inside cobalt/, exports git format-patch here
just patch-refresh        # re-export all patches from the cobalt/ git history, regenerate `series`
just sync                 # gclient sync -r $(cat DEPS.pin), then re-apply this series
```

## Rules (doc §13.1)

- **C++17 only** (the in-tree toolchain rule in `CLAUDE.md`): newer language features maximize
  rebase pain.
- Compile against the tree's hermetic Clang + in-tree libc++; format with
  `buildtools/linux64/clang-format` (`just fmt`).
- Keep each patch small and single-purpose so each Cobalt bump stays cheap.
