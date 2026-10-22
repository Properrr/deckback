#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace deckback {

// The launcher's one outbound HTTP client (libcurl, optional at build time). Not used for CDP,
// which devtools.cpp speaks over raw sockets.

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

inline std::optional<std::string> http_get(std::string_view url, long timeout_seconds,
                                           std::string_view user_agent) {
  return http_get(HttpRequest{std::string(url), timeout_seconds, std::string(user_agent), {}});
}

// Whether this build has libcurl. When false, every http_get() returns nullopt.
bool http_available();

}  // namespace deckback
