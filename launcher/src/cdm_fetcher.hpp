#pragma once
#include <string>

namespace deckback {

// Where to fetch the Widevine CDM from, if the user opted in. Empty `url` (the default) = no
// download, only detection. `url` points at a raw `libwidevinecdm.so`; `sha256` is the pinned
// lowercase hex digest (empty = unverified, logged as a risk).
struct CdmConfig {
  std::string url;
  std::string sha256;
};

// First-run Widevine CDM install (best-effort). The CDM is NEVER redistributed by this project
// (docs/legal.md); at most it is fetched from a user-configured URL and hash-verified.
class CdmFetcher {
 public:
  // True if a valid CDM is already installed under `profile_dir`, or was fetched+verified+installed
  // from `cfg`. False (non-fatal) otherwise, with an actionable log line.
  static bool ensure_installed(const std::string& profile_dir, const CdmConfig& cfg = {});

  // Where the launcher installs the CDM under the profile dir (Chrome-like layout, without the
  // version subdir and manifest.json). The engine finds it only via the CDM registration patch.
  static std::string installed_path(const std::string& profile_dir);

  // Would the engine accept this path on `--widevine-cdm-path`? Mirrors the rule in
  // `patches/0001-…-widevine-cdm-registration.patch`: absolute, no `..` components. The engine
  // stays authoritative; this only lets the launcher log the refusal.
  static bool usable_cdm_path(const std::string& path);

  static bool backend_available();

  // What is at `dest` before anything is fetched; checked on every launch, so a pinned CDM that
  // no longer matches its hash is re-fetched.
  enum class Installed { Missing, Unpinned, Verified, Mismatch };
  static Installed check_installed(const std::string& dest, const std::string& sha256);

  // Verify `bytes` against `sha256` (empty = unverified, logged) and write them to `dest`
  // atomically: the old file stays whole until the new one has fully landed.
  static bool install(const std::string& dest, const std::string& bytes, const std::string& sha256);
};

}  // namespace deckback
