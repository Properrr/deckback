#include "cdm_fetcher.hpp"

#include "fileio.hpp"
#include "http.hpp"
#include "log.hpp"
#include "sha256.hpp"

namespace deckback {
namespace {

constexpr long kDownloadTimeoutSec = 120;  // a CDM is several megabytes

}  // namespace

bool CdmFetcher::backend_available() { return http_available(); }

std::string CdmFetcher::installed_path(const std::string& profile_dir) {
  return profile_dir + "/WidevineCdm/_platform_specific/linux_x64/libwidevinecdm.so";
}

bool CdmFetcher::usable_cdm_path(const std::string& path) {
  // `starts_with` is total, so the empty string is rejected here too.
  if (!path.starts_with('/')) return false;
  // Reject `..` as a whole component only: "a/../b" fails; "a..b" and "..x" are legal names.
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string::npos) end = path.size();
    if (path.compare(start, end - start, "..") == 0) return false;
    start = end + 1;
  }
  return true;
}

CdmFetcher::Installed CdmFetcher::check_installed(const std::string& dest,
                                                  const std::string& sha256) {
  if (!file_exists(dest)) return Installed::Missing;
  if (sha256.empty()) return Installed::Unpinned;
  const auto bytes = read_file(dest);
  return bytes && sha256_hex(*bytes) == sha256 ? Installed::Verified : Installed::Mismatch;
}

bool CdmFetcher::install(const std::string& dest, const std::string& bytes,
                         const std::string& sha256) {
  if (!sha256.empty()) {
    const std::string got = sha256_hex(bytes);
    if (got != sha256) {
      warn("cdm: sha256 mismatch (got " + got + ", expected " + sha256 + ") — refusing to install");
      return false;
    }
    info("cdm: sha256 verified");
  } else {
    warn("cdm: no cdm_sha256 configured — installing an UNVERIFIED CDM binary (set cdm_sha256)");
  }
  if (!write_file_atomic(dest, bytes)) {
    warn("cdm: could not write " + dest);
    return false;
  }
  info("cdm: installed " + dest);
  return true;
}

bool CdmFetcher::ensure_installed(const std::string& profile_dir, const CdmConfig& cfg) {
  const std::string dest = installed_path(profile_dir);
  const Installed have = check_installed(dest, cfg.sha256);
  switch (have) {
    case Installed::Verified:
      info("cdm: Widevine CDM present, sha256 verified (" + dest + ")");
      return true;
    case Installed::Unpinned:
      info("cdm: Widevine CDM present (" + dest + "; no cdm_sha256 to verify it against)");
      return true;
    case Installed::Mismatch:
      // Truncated, or the pin moved to a new CDM: fetch the pinned one.
      warn("cdm: installed CDM does not match cdm_sha256 — fetching the pinned one");
      break;
    case Installed::Missing:
      break;
  }
  // On a failed fetch a mismatched CDM is still used: the atomic install leaves it in place, and a
  // moved pin must not strand a working CDM.
  const bool fallback = have == Installed::Mismatch;

  if (cfg.url.empty()) {
    if (fallback) {
      warn("cdm: no cdm_url to fetch the pinned CDM from — using the installed one as it is");
      return true;
    }
    // No opt-in URL: we must never fetch or bundle Google's CDM ourselves (docs/legal.md).
    info(
        "cdm: no Widevine CDM installed and no cdm_url configured — DRM/paid content will be "
        "unavailable; free YouTube is unaffected. Set cdm_url + cdm_sha256 in app.json to fetch "
        "one.");
    return false;
  }

  if (!http_available()) {
    warn("cdm: cdm_url set but this build has no libcurl — cannot fetch");
    return fallback;
  }
  info("cdm: fetching Widevine CDM from " + cfg.url);
  const auto downloaded = http_get(cfg.url, kDownloadTimeoutSec, "deckback-cdm-fetcher/1");
  if (!downloaded) {
    warn("cdm: download failed — DRM unavailable; free YouTube is unaffected");
    return fallback;
  }
  return install(dest, *downloaded, cfg.sha256) || fallback;
}

}  // namespace deckback
