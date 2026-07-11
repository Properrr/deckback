#include "cdm_fetcher.hpp"

#include <sys/stat.h>

#include <fstream>
#include <sstream>

#include "log.hpp"
#include "sha256.hpp"

#if __has_include(<curl/curl.h>)
#define DECKBACK_HAVE_CURL 1
#include <curl/curl.h>
#else
#define DECKBACK_HAVE_CURL 0
#endif

namespace deckback {
namespace {

bool file_exists(const std::string& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// mkdir -p for every parent of `path` (the filename component is ignored).
void make_parent_dirs(const std::string& path) {
  for (size_t i = 1; i < path.size(); ++i) {
    if (path[i] != '/') continue;
    const std::string dir = path.substr(0, i);
    ::mkdir(dir.c_str(), 0700);  // ignore EEXIST
  }
}

#if DECKBACK_HAVE_CURL
size_t sink(char* ptr, size_t size, size_t nmemb, void* user) {
  auto* out = static_cast<std::string*>(user);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

// GET `url` into `out`. Returns false on any transport/HTTP error.
bool http_download(const std::string& url, std::string& out) {
  CURL* c = curl_easy_init();
  if (!c) return false;
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "deckback-cdm-fetcher/1");
  const CURLcode rc = curl_easy_perform(c);
  curl_easy_cleanup(c);
  return rc == CURLE_OK && !out.empty();
}
#endif

bool write_file(const std::string& path, const std::string& bytes) {
  make_parent_dirs(path);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return f.good();
}

}  // namespace

bool CdmFetcher::backend_available() { return DECKBACK_HAVE_CURL != 0; }

std::string CdmFetcher::installed_path(const std::string& profile_dir) {
  return profile_dir + "/WidevineCdm/_platform_specific/linux_x64/libwidevinecdm.so";
}

bool CdmFetcher::usable_cdm_path(const std::string& path) {
  // `starts_with` rather than `front() != '/'`: it is total, so the empty string falls out for
  // free. The `path.empty()` guard it replaces was unkillable by any test — `front()` on an empty
  // string is UB that happens to return '\0' — and a check no test can fail is not a check.
  if (!path.starts_with('/')) return false;
  // `..` anywhere in the path, as a whole component. "a/../b" is rejected; "a..b" and "..x" are not
  // path traversal and are perfectly legal directory names.
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string::npos) end = path.size();
    if (path.compare(start, end - start, "..") == 0) return false;
    start = end + 1;
  }
  return true;
}

bool CdmFetcher::ensure_installed(const std::string& profile_dir, const CdmConfig& cfg) {
  const std::string dest = installed_path(profile_dir);
  if (file_exists(dest)) {
    info("cdm: Widevine CDM present (" + dest + ")");
    return true;
  }

  if (cfg.url.empty()) {
    // No opt-in URL: we must not fetch or bundle Google's CDM ourselves (docs/legal.md). Tell the
    // user how to enable it and make clear free YouTube still works.
    info(
        "cdm: no Widevine CDM installed and no cdm_url configured — DRM/paid content will be "
        "unavailable; free YouTube is unaffected. Set cdm_url + cdm_sha256 in app.json to fetch "
        "one.");
    return false;
  }

#if DECKBACK_HAVE_CURL
  info("cdm: fetching Widevine CDM from " + cfg.url);
  std::string bytes;
  if (!http_download(cfg.url, bytes)) {
    warn("cdm: download failed — DRM unavailable; free YouTube is unaffected");
    return false;
  }
  if (!cfg.sha256.empty()) {
    const std::string got = sha256_hex(bytes);
    if (got != cfg.sha256) {
      warn("cdm: sha256 mismatch (got " + got + ", expected " + cfg.sha256 +
           ") — refusing to install");
      return false;
    }
    info("cdm: sha256 verified");
  } else {
    warn("cdm: no cdm_sha256 configured — installing an UNVERIFIED CDM binary (set cdm_sha256)");
  }
  if (!write_file(dest, bytes)) {
    warn("cdm: could not write " + dest);
    return false;
  }
  info("cdm: installed " + dest);
  return true;
#else
  warn("cdm: cdm_url set but this build has no libcurl — cannot fetch");
  return false;
#endif
}

}  // namespace deckback
