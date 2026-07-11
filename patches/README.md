# patches/

A rebaseable quilt-style series of patches applied on top of the pinned Cobalt commit
(`DEPS.pin`). **Never edit `cobalt/` in place and leave it** — the checkout is gitignored and
`just sync` will re-apply this series over a clean tree.

**The series holds exactly one patch, and that is the design working.** Gamepad input, the
`MediaCapabilities` AV1 steering, and the mic auto-grant were all planned as patches; every one of
them landed in `launcher/` over the DevTools protocol instead. Prefer the launcher: a patch is a
recurring rebase cost, a CDP call is not.

## `0001-…-widevine-cdm-registration`

The one genuinely unavoidable patch, because the engine offers no other seam. `content_shell` does
not override `ContentClient::AddContentDecryptionModules`, so the `CdmRegistry` is empty and every
`com.widevine.alpha` request is rejected no matter what `enable_widevine` says. There is no CDP call,
no command-line switch, and no runtime hook that fills it.

Two facts, both discovered by reading the tree and both easy to get wrong:

- **The capability must be concrete at registration.** Upstream lazily initialises the software-secure
  capability from the CDM's `manifest.json`. On Linux, `CdmRegistryImpl::LazyInitializeCapability()`
  falls into the `#else` branch and hands back `absl::nullopt` — the comment right there reads
  *"kSoftwareSecure should have been determined from the manifest"*. Register with an empty optional
  and the CDM loads, the registry accepts it, and `requestMediaKeySystemAccess` **still rejects**,
  with nothing in the log to explain why.
- **`enable_library_cdms` is already `true`** (it is `toolkit_views && !is_castos`). The note in
  `args/deck.gn` suggesting you may need to add it is obsolete; only `enable_widevine = true` is ours.

The CDM path arrives on `--widevine-cdm-path`, which the launcher passes from
`CdmFetcher::installed_path()` and **only when a CDM is actually installed**. We never bundle or
redistribute Google's CDM (`docs/legal.md`); unencrypted YouTube is unaffected by its absence.

Verified 2026-07-09: the patch applies cleanly to a pristine `DEPS.pin` tree, `gn gen` accepts the
new dep, and `shell_content_client.o` **compiles** under the `deck` preset with `ENABLE_WIDEVINE()`
live (the symbol is in the object file). **Not verified:** that a real CDM loads, that a licence
exchange succeeds, or that any DRM video plays. That needs a CDM on a Deck, and
`tests/deck/test_media.py::test_widevine_state` is the test that will say so.

## Workflow

```sh
just patch-new <name>     # after committing your change inside cobalt/, exports git format-patch here
just patch-refresh        # re-export all patches from the cobalt/ git history, regenerate `series`
just sync                 # gclient sync -r $(cat DEPS.pin), then re-apply this series
```

## Rules (doc §13.1)

- **C++17 only.** M114 predates Chromium's C++20 migration; C++20 breaks the toolchain gate and
  maximizes rebase pain.
- Compile against the tree's hermetic Clang + in-tree libc++; format with
  `buildtools/linux64/clang-format` (`just fmt`).
- Keep each patch small and single-purpose so the monthly trunk rebase stays cheap.
