#pragma once
#include <string>

namespace deckback {

// Where to fetch the Widevine CDM from, if the user opted in via config. Both empty (the default) =
// no auto-download: the fetcher only detects an already-installed CDM and otherwise prints an
// honest message. `url` must point at a raw `libwidevinecdm.so` (a mirror the user trusts);
// `sha256` is the pinned lowercase hex digest it is verified against (empty = skip verification,
// logged as a risk).
struct CdmConfig {
  std::string url;
  std::string sha256;
};

// First-run Widevine CDM path (doc §6 Phase 7, best-effort). The CDM binary is NEVER redistributed
// by this project (docs/legal.md) — at most it is fetched from a user-configured URL and
// hash-verified. Free YouTube is unaffected whether or not a CDM is present. Download is backed by
// libcurl.
class CdmFetcher {
 public:
  // True if a valid CDM is already installed under `profile_dir`, or was fetched+verified+installed
  // from `cfg`. False (non-fatal) otherwise, with an actionable log line.
  static bool ensure_installed(const std::string& profile_dir, const CdmConfig& cfg = {});

  // Where the launcher installs the CDM under the profile dir (Chrome-like layout, minus Chrome's
  // version subdir + manifest.json). The engine does not discover this on its own — the P7
  // registration patch must point the CdmRegistry here (m114.md "Widevine registration gap").
  static std::string installed_path(const std::string& profile_dir);

  // Would the engine accept this path on `--widevine-cdm-path`? Mirrors the rule in
  // `patches/0001-…-widevine-cdm-registration.patch`: absolute, no parent references.
  //
  // The engine is authoritative and rejects a bad path itself. This exists so the refusal is logged
  // by the launcher, next to the profile dir that caused it, instead of surfacing as one LOG(ERROR)
  // in the engine's stderr with no mention of where the path came from. A profile dir can be made
  // relative by `DECKBACK_PROFILE=./foo`.
  static bool usable_cdm_path(const std::string& path);

  static bool backend_available();
};

}  // namespace deckback
