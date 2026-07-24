#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace deckback {

// The launcher's one outbound HTTP client. Two callers exist — the CDM fetcher (a large binary, on
// the startup path) and the update prompt's changelog fetch (a small JSON body, on a worker thread)
// — and they used to carry a libcurl wrapper each: two `__has_include(<curl/curl.h>)` guards, two
// write callbacks, two `curl_easy_setopt` lists that had already drifted apart on timeout and
// User-Agent. Transport policy (redirects, TLS, timeouts, what counts as failure) belongs in one
// place.
//
// NOTE: this is deliberately NOT the CDP transport. devtools.cpp speaks raw sockets + its own
// RFC 6455 WebSocket to loopback, because libcurl's WS support is experimental and the launcher
// must work without libcurl at all.

struct HttpRequest {
  std::string url;
  long timeout_seconds = 30;
  // Sent verbatim as User-Agent. GitHub's API rejects a request without one.
  std::string user_agent = "deckback/1";
  // Extra headers, each "Name: value".
  std::vector<std::string> headers;
};

// GET `req` and return the body. nullopt on any transport error, on an HTTP status >= 400, on an
// empty body, or when this build has no libcurl. Blocking; the caller owns which thread it runs on.
std::optional<std::string> http_get(const HttpRequest& req);

// Convenience for the common case.
inline std::optional<std::string> http_get(std::string_view url, long timeout_seconds,
                                           std::string_view user_agent) {
  return http_get(HttpRequest{std::string(url), timeout_seconds, std::string(user_agent), {}});
}

// Whether this build can make HTTP requests at all (libcurl present at compile time). False makes
// every http_get() return nullopt — the CDM fetch and the changelog both degrade to a log line.
bool http_available();

}  // namespace deckback
