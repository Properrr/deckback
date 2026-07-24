#include "http.hpp"

#if __has_include(<curl/curl.h>)
#define DECKBACK_HAVE_CURL 1
#include <curl/curl.h>
#else
#define DECKBACK_HAVE_CURL 0
#endif

namespace deckback {
namespace {

#if DECKBACK_HAVE_CURL
size_t sink(char* ptr, size_t size, size_t nmemb, void* user) {
  static_cast<std::string*>(user)->append(ptr, size * nmemb);
  return size * nmemb;
}
#endif

}  // namespace

bool http_available() { return DECKBACK_HAVE_CURL != 0; }

#if DECKBACK_HAVE_CURL
std::optional<std::string> http_get(const HttpRequest& req) {
  CURL* c = curl_easy_init();
  if (!c) return std::nullopt;

  std::string body;
  curl_slist* hdrs = nullptr;
  for (const std::string& h : req.headers) hdrs = curl_slist_append(hdrs, h.c_str());

  curl_easy_setopt(c, CURLOPT_URL, req.url.c_str());
  if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
  // Turn a 4xx/5xx into a transport failure. Without it a 404 body — an HTML error page, or
  // GitHub's rate-limit JSON — is returned as if it were the thing that was asked for, and the CDM
  // fetcher would hash and install it.
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, req.timeout_seconds);
  curl_easy_setopt(c, CURLOPT_USERAGENT, req.user_agent.c_str());

  const CURLcode rc = curl_easy_perform(c);
  if (hdrs) curl_slist_free_all(hdrs);
  curl_easy_cleanup(c);

  if (rc != CURLE_OK || body.empty()) return std::nullopt;
  return body;
}
#else
std::optional<std::string> http_get(const HttpRequest&) { return std::nullopt; }
#endif

}  // namespace deckback
