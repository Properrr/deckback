#include "devtools.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <format>

#include "log.hpp"

namespace deckback {
namespace ws {
namespace {

constexpr size_t kMaxPayload = 32 * 1024 * 1024;  // CDP replies are tiny; cap guards a resize bomb.

}  // namespace

std::string encode_frame(uint8_t opcode, std::string_view payload, bool masked, uint32_t mask,
                         bool fin) {
  std::string out;
  out.push_back(static_cast<char>((fin ? 0x80 : 0x00) | (opcode & 0x0F)));

  const uint8_t mask_bit = masked ? 0x80 : 0x00;
  const size_t len = payload.size();
  if (len < 126) {
    out.push_back(static_cast<char>(mask_bit | static_cast<uint8_t>(len)));
  } else if (len <= 0xFFFF) {
    out.push_back(static_cast<char>(mask_bit | 126));
    out.push_back(static_cast<char>((len >> 8) & 0xFF));
    out.push_back(static_cast<char>(len & 0xFF));
  } else {
    out.push_back(static_cast<char>(mask_bit | 127));
    for (int i = 7; i >= 0; --i)
      out.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (8 * i)) & 0xFF));
  }

  std::array<uint8_t, 4> mk{static_cast<uint8_t>(mask >> 24), static_cast<uint8_t>(mask >> 16),
                            static_cast<uint8_t>(mask >> 8), static_cast<uint8_t>(mask)};
  if (masked)
    for (uint8_t b : mk) out.push_back(static_cast<char>(b));
  for (size_t i = 0; i < len; ++i) {
    uint8_t b = static_cast<uint8_t>(payload[i]);
    out.push_back(static_cast<char>(masked ? (b ^ mk[i % 4]) : b));
  }
  return out;
}

DecodeResult decode_frame(std::string_view buf) {
  DecodeResult r;
  auto need_more = [&] {
    r.status = DecodeResult::NeedMore;
    return r;
  };
  if (buf.size() < 2) return need_more();

  auto u = [&](size_t i) { return static_cast<uint8_t>(buf[i]); };
  const bool fin = u(0) & 0x80;
  const uint8_t opcode = u(0) & 0x0F;
  const bool masked = u(1) & 0x80;
  uint64_t len = u(1) & 0x7F;
  size_t pos = 2;

  if (len == 126) {
    if (buf.size() < pos + 2) return need_more();
    len = (static_cast<uint64_t>(u(pos)) << 8) | u(pos + 1);
    pos += 2;
  } else if (len == 127) {
    if (buf.size() < pos + 8) return need_more();
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | u(pos + i);
    pos += 8;
  }
  if (len > kMaxPayload) {
    r.status = DecodeResult::Error;
    return r;
  }

  std::array<uint8_t, 4> mk{0, 0, 0, 0};
  if (masked) {
    if (buf.size() < pos + 4) return need_more();
    for (int i = 0; i < 4; ++i) mk[i] = u(pos + i);
    pos += 4;
  }
  if (buf.size() < pos + len) return need_more();

  r.frame.fin = fin;
  r.frame.opcode = opcode;
  r.frame.payload.resize(len);
  for (uint64_t i = 0; i < len; ++i)
    r.frame.payload[i] = static_cast<char>(masked ? (u(pos + i) ^ mk[i % 4]) : u(pos + i));
  r.consumed = pos + len;
  r.status = DecodeResult::Ok;
  return r;
}

}  // namespace ws

namespace {

long mono_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1'000'000L;
}

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const unsigned char* data, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t b = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < n) b |= static_cast<uint32_t>(data[i + 1]) << 8;
    if (i + 2 < n) b |= data[i + 2];
    out.push_back(kB64[(b >> 18) & 0x3F]);
    out.push_back(kB64[(b >> 12) & 0x3F]);
    out.push_back(i + 1 < n ? kB64[(b >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < n ? kB64[b & 0x3F] : '=');
  }
  return out;
}

// JSON-escape a string for embedding in a CDP expression literal.
std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

// Reverse json_escape for a value pulled out of a CDP reply (URLs, titles). Handles the escapes CDP
// actually emits; a stray backslash is passed through. Sufficient for our primitive string reads.
std::string json_unescape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' || i + 1 >= s.size()) {
      out.push_back(s[i]);
      continue;
    }
    switch (s[++i]) {
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case '"':
        out.push_back('"');
        break;
      case '\\':
        out.push_back('\\');
        break;
      case '/':
        out.push_back('/');
        break;
      case 'u': {
        if (i + 4 < s.size()) {
          int cp = 0;
          bool ok = true;
          for (int j = 1; j <= 4; ++j) {
            char c = s[i + j];
            cp <<= 4;
            if (c >= '0' && c <= '9')
              cp |= c - '0';
            else if (c >= 'a' && c <= 'f')
              cp |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
              cp |= c - 'A' + 10;
            else {
              ok = false;
              break;
            }
          }
          if (ok) {
            i += 4;
            if (cp < 0x80) {
              out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
              out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
              out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
              out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
              out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
              out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            break;
          }
        }
        out.push_back('\\');
        out.push_back('u');
        break;
      }
      default:
        out.push_back('\\');
        out.push_back(s[i]);
    }
  }
  return out;
}

// Parse the top-level `"id":<n>` of a CDP reply. nullopt for events (which carry no id). Parsing
// the integer (rather than substring-matching) avoids `"id":4` false-matching inside `"id":42`.
std::optional<uint64_t> response_id(const std::string& body) {
  size_t k = body.find("\"id\":");
  if (k == std::string::npos) return std::nullopt;
  size_t p = k + 5;
  while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
  uint64_t id = 0;
  bool any = false;
  while (p < body.size() && body[p] >= '0' && body[p] <= '9') {
    id = id * 10 + static_cast<uint64_t>(body[p] - '0');
    ++p;
    any = true;
  }
  return any ? std::optional<uint64_t>(id) : std::nullopt;
}

// Find `"value":` in a CDP RemoteObject reply and return the raw token that follows (true/false/a
// number/a quoted string). Our expressions only ever return primitives, so the first match is ours.
std::optional<std::string> extract_value_token(const std::string& body) {
  size_t k = body.find("\"value\":");
  if (k == std::string::npos) return std::nullopt;
  size_t p = k + 8;
  while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
  if (p >= body.size()) return std::nullopt;
  if (body[p] == '"') {
    size_t end = p + 1;
    while (end < body.size() && body[end] != '"') {
      if (body[end] == '\\') ++end;
      ++end;
    }
    return body.substr(p, end - p + 1);
  }
  size_t end = p;
  while (end < body.size() && body[end] != ',' && body[end] != '}') ++end;
  std::string tok = body.substr(p, end - p);
  while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
  return tok;
}

int connect_tcp(const std::string& host, int port, int timeout_ms) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string port_s = std::to_string(port);
  if (getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0 || !res) return -1;

  int fd = -1;
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc == 0) {
      fcntl(fd, F_SETFL, flags);
      break;
    }
    if (errno == EINPROGRESS) {
      pollfd pfd{fd, POLLOUT, 0};
      if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT)) {
        int err = 0;
        socklen_t l = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
        if (err == 0) {
          fcntl(fd, F_SETFL, flags);
          break;
        }
      }
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  return fd;
}

// Case-insensitive Content-Length lookup in a response header block. -1 when absent.
long content_length_of(std::string_view headers) {
  constexpr std::string_view kKey = "content-length:";
  std::string lower(headers);
  for (char& c : lower)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  size_t k = lower.find(kKey);
  if (k == std::string::npos) return -1;
  size_t p = k + kKey.size();
  while (p < lower.size() && (lower[p] == ' ' || lower[p] == '\t')) ++p;
  long v = 0;
  bool any = false;
  while (p < lower.size() && lower[p] >= '0' && lower[p] <= '9') {
    v = v * 10 + (lower[p] - '0');
    ++p;
    any = true;
  }
  return any ? v : -1;
}

// Blocking HTTP/1.1 GET. Returns once the Content-Length body has arrived (Chromium's DevTools HTTP
// server keeps the socket open despite our `Connection: close`, so reading-until-close would block
// until the poll timeout and fail). Falls back to read-until-close when no Content-Length is sent.
// Returns the full response (headers + body), or nullopt on failure.
std::optional<std::string> http_get(int fd, const std::string& host, int port,
                                    const std::string& path) {
  const std::string req = std::format(
      "GET {} HTTP/1.1\r\nHost: {}:{}\r\nAccept: application/json\r\nConnection: close\r\n\r\n",
      path, host, port);
  if (::send(fd, req.data(), req.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(req.size()))
    return std::nullopt;

  std::string resp;
  char buf[4096];
  size_t header_end = std::string::npos;
  long content_length = -1;
  const long deadline = mono_ms() + 2000;
  for (;;) {
    if (header_end == std::string::npos) {
      if (size_t h = resp.find("\r\n\r\n"); h != std::string::npos) {
        header_end = h + 4;
        content_length = content_length_of(std::string_view(resp).substr(0, header_end));
      }
    }
    if (header_end != std::string::npos && content_length >= 0 &&
        resp.size() >= header_end + static_cast<size_t>(content_length))
      return resp;  // full body in hand — don't wait for a close that may never come

    long left = deadline - mono_ms();
    if (left <= 0) return header_end != std::string::npos ? std::optional(resp) : std::nullopt;
    pollfd pfd{fd, POLLIN, 0};
    if (poll(&pfd, 1, static_cast<int>(left)) <= 0)
      return header_end != std::string::npos ? std::optional(resp) : std::nullopt;
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }
    if (n == 0) return resp.empty() ? std::nullopt : std::optional(resp);  // peer closed
    resp.append(buf, static_cast<size_t>(n));
    if (resp.size() > ws::kMaxPayload) return std::nullopt;
  }
}

}  // namespace

DevToolsClient::DevToolsClient(std::string host, int port)
    : host_(std::move(host)), port_(port), rng_(std::random_device{}()) {}

DevToolsClient::~DevToolsClient() {
  std::lock_guard lk(mutex_);
  disconnect();
}

void DevToolsClient::disconnect() {
  if (fd_ >= 0) close(fd_);
  fd_ = -1;
  rxbuf_.clear();
}

bool DevToolsClient::connected() {
  std::lock_guard lk(mutex_);
  return fd_ >= 0;
}

// GET /json/list, then pull the path out of the first target's webSocketDebuggerUrl.
std::optional<std::string> DevToolsClient::discover_ws_path() {
  int fd = connect_tcp(host_, port_, 1500);
  if (fd < 0) return std::nullopt;
  auto resp = http_get(fd, host_, port_, "/json/list");
  close(fd);
  if (!resp) return std::nullopt;

  size_t k = resp->find("\"webSocketDebuggerUrl\"");
  if (k == std::string::npos) return std::nullopt;
  size_t q = resp->find('"', resp->find(':', k) + 1);
  if (q == std::string::npos) return std::nullopt;
  size_t end = resp->find('"', q + 1);
  if (end == std::string::npos) return std::nullopt;
  const std::string url = resp->substr(q + 1, end - q - 1);  // ws://host:port/devtools/page/ID

  size_t scheme = url.find("://");
  if (scheme == std::string::npos) return std::nullopt;
  size_t slash = url.find('/', scheme + 3);
  if (slash == std::string::npos) return "/";
  return url.substr(slash);
}

bool DevToolsClient::ws_handshake(const std::string& path) {
  unsigned char key_raw[16];
  for (unsigned char& b : key_raw) b = static_cast<unsigned char>(rng_() & 0xFF);
  const std::string key = base64(key_raw, sizeof key_raw);
  const std::string req = std::format(
      "GET {} HTTP/1.1\r\nHost: {}:{}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Key: {}\r\nSec-WebSocket-Version: 13\r\n\r\n",
      path, host_, port_, key);
  if (!send_all(req)) return false;

  // Read until the end of the handshake response headers.
  rxbuf_.clear();
  const long deadline = mono_ms() + 2000;
  while (rxbuf_.find("\r\n\r\n") == std::string::npos) {
    long left = deadline - mono_ms();
    if (left <= 0) return false;
    pollfd pfd{fd_, POLLIN, 0};
    if (poll(&pfd, 1, static_cast<int>(left)) <= 0) return false;
    char buf[2048];
    ssize_t n = recv(fd_, buf, sizeof buf, 0);
    if (n <= 0) return false;
    rxbuf_.append(buf, static_cast<size_t>(n));
    if (rxbuf_.size() > 64 * 1024) return false;
  }
  size_t header_end = rxbuf_.find("\r\n\r\n") + 4;
  const std::string status = rxbuf_.substr(0, rxbuf_.find("\r\n"));
  if (status.find(" 101") == std::string::npos) {
    warn("devtools: WebSocket upgrade rejected: " + status);
    return false;
  }
  rxbuf_.erase(0, header_end);  // any bytes past the headers are the first WS frame(s)
  return true;
}

bool DevToolsClient::ensure_connected() {
  if (fd_ >= 0) return true;
  auto path = discover_ws_path();
  if (!path) {
    if (!warned_unreachable_) {
      warn(std::format("devtools: no debug target on {}:{} (is remote-debugging-port set?)", host_,
                       port_));
      warned_unreachable_ = true;
    }
    return false;
  }
  fd_ = connect_tcp(host_, port_, 1500);
  if (fd_ < 0) return false;
  if (!ws_handshake(*path)) {
    disconnect();
    return false;
  }
  warned_unreachable_ = false;
  info("devtools: WebSocket session established");
  // Re-arm the sticky state on the fresh target. Leanback tears the page target down on navigation,
  // and a new target reverts to content_shell defaults — without this the app would fall back to
  // the desktop redirect (UA) or lose codec steering / the mic grant after the first navigation.
  // request() below sees fd_>=0 and won't recurse.
  if (!sticky_ua_.empty()) apply_user_agent();
  for (const std::string& s : sticky_scripts_) install_script(s);
  if (!grant_origin_.empty()) apply_grant();
  return true;
}

bool DevToolsClient::apply_user_agent() {
  if (sticky_ua_.empty()) return true;
  request("Network.enable", "{}");
  return request("Network.setUserAgentOverride",
                 std::format(R"({{"userAgent":"{}"}})", json_escape(sticky_ua_)))
      .has_value();
}

bool DevToolsClient::install_script(std::string_view source) {
  request("Page.enable", "{}");
  return request("Page.addScriptToEvaluateOnNewDocument",
                 std::format(R"({{"source":"{}"}})", json_escape(source)))
      .has_value();
}

bool DevToolsClient::apply_grant() {
  if (grant_origin_.empty()) return true;
  return request("Browser.grantPermissions",
                 std::format(R"({{"origin":"{}","permissions":["{}"]}})",
                             json_escape(grant_origin_), json_escape(grant_perm_)))
      .has_value();
}

bool DevToolsClient::send_all(const std::string& bytes) {
  size_t off = 0;
  while (off < bytes.size()) {
    ssize_t n = ::send(fd_, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

std::optional<ws::Frame> DevToolsClient::read_frame(int deadline_ms) {
  for (;;) {
    ws::DecodeResult dr = ws::decode_frame(rxbuf_);
    if (dr.status == ws::DecodeResult::Error) return std::nullopt;
    if (dr.status == ws::DecodeResult::Ok) {
      rxbuf_.erase(0, dr.consumed);
      return dr.frame;
    }
    long left = deadline_ms - mono_ms();
    if (left <= 0) return std::nullopt;
    pollfd pfd{fd_, POLLIN, 0};
    if (poll(&pfd, 1, static_cast<int>(left)) <= 0) return std::nullopt;
    char buf[4096];
    ssize_t n = recv(fd_, buf, sizeof buf, 0);
    if (n <= 0) return std::nullopt;
    rxbuf_.append(buf, static_cast<size_t>(n));
  }
}

std::optional<std::string> DevToolsClient::request(std::string_view method,
                                                   std::string_view params_json) {
  if (!ensure_connected()) return std::nullopt;

  const uint64_t id = ++next_id_;
  const std::string msg =
      std::format(R"({{"id":{},"method":"{}","params":{}}})", id, method, params_json);
  const uint32_t mask = rng_();
  if (!send_all(ws::encode_frame(ws::kText, msg, /*masked=*/true, mask))) {
    disconnect();
    return std::nullopt;
  }

  const long deadline = mono_ms() + 2500;
  for (;;) {
    auto frame = read_frame(static_cast<int>(deadline));
    if (!frame) {
      disconnect();
      return std::nullopt;
    }
    switch (frame->opcode) {
      case ws::kPing:
        send_all(ws::encode_frame(ws::kPong, frame->payload, true, rng_()));
        continue;
      case ws::kClose:
        disconnect();
        return std::nullopt;
      case ws::kText:
        break;
      default:
        continue;  // pong / binary / continuation — ignore
    }
    const std::string& body = frame->payload;
    auto rid = response_id(body);
    if (!rid || *rid != id) continue;  // a CDP event (no id) or a stale response
    if (body.find("\"exceptionDetails\"") != std::string::npos ||
        body.find("\"error\":") != std::string::npos) {
      warn(std::format("devtools: {} returned an error", method));
      return std::nullopt;
    }
    return body;
  }
}

std::optional<std::string> DevToolsClient::evaluate(std::string_view expression) {
  return request("Runtime.evaluate",
                 std::format(R"({{"expression":"{}","returnByValue":true,"timeout":1500}})",
                             json_escape(expression)));
}

std::optional<bool> DevToolsClient::eval_bool(std::string_view expression) {
  std::lock_guard lk(mutex_);
  auto body = evaluate(expression);
  if (!body) return std::nullopt;
  auto tok = extract_value_token(*body);
  if (!tok) return std::nullopt;
  if (*tok == "true") return true;
  if (*tok == "false") return false;
  return std::nullopt;
}

std::optional<double> DevToolsClient::eval_number(std::string_view expression) {
  std::lock_guard lk(mutex_);
  auto body = evaluate(expression);
  if (!body) return std::nullopt;
  auto tok = extract_value_token(*body);
  if (!tok || tok->empty()) return std::nullopt;
  try {
    return std::stod(*tok);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::string> DevToolsClient::eval_string(std::string_view expression) {
  std::lock_guard lk(mutex_);
  auto body = evaluate(expression);
  if (!body) return std::nullopt;
  auto tok = extract_value_token(*body);
  if (!tok || tok->size() < 2 || tok->front() != '"' || tok->back() != '"') return std::nullopt;
  return json_unescape(std::string_view(*tok).substr(1, tok->size() - 2));
}

bool DevToolsClient::eval_void(std::string_view expression) {
  std::lock_guard lk(mutex_);
  return evaluate(expression).has_value();
}

bool DevToolsClient::set_user_agent_override(std::string_view ua) {
  std::lock_guard lk(mutex_);
  sticky_ua_.assign(ua);
  return apply_user_agent();
}

bool DevToolsClient::navigate(std::string_view url) { return navigate_checked(url).sent; }

DevToolsClient::NavStatus DevToolsClient::navigate_checked(std::string_view url) {
  std::lock_guard lk(mutex_);
  request("Page.enable", "{}");
  auto body = request("Page.navigate", std::format(R"({{"url":"{}"}})", json_escape(url)));
  if (!body) return {};

  NavStatus st;
  st.sent = true;
  // Page.navigate answers with `errorText` on a failed navigation and omits the field on success —
  // it does not report failure as a CDP error, so `request()` returning a body proves nothing.
  const std::string needle = "\"errorText\":\"";
  auto at = body->find(needle);
  if (at != std::string::npos) {
    const size_t start = at + needle.size();
    // Find the closing quote, honouring backslash escapes.
    size_t end = start;
    while (end < body->size() && (*body)[end] != '"') end += ((*body)[end] == '\\') ? 2 : 1;
    if (end <= body->size())
      st.error_text = json_unescape(std::string_view(*body).substr(start, end - start));
  }
  return st;
}

bool DevToolsClient::reload() {
  std::lock_guard lk(mutex_);
  request("Page.enable", "{}");
  return request("Page.reload", R"({"ignoreCache":true})").has_value();
}

bool DevToolsClient::add_script_on_new_document(std::string_view source) {
  std::lock_guard lk(mutex_);
  sticky_scripts_.emplace_back(source);
  return install_script(source);
}

bool DevToolsClient::grant_permissions(std::string_view origin, std::string_view permission) {
  std::lock_guard lk(mutex_);
  grant_origin_.assign(origin);
  grant_perm_.assign(permission);
  return apply_grant();
}

namespace {
// A fully-resolved key event: what Blink needs to synthesize a trusted keydown/keyup pair.
// `text` is non-empty only for printable keys — Blink derives the charCode from it, which is what
// makes `beforeinput`/`textInput` fire and a character land in a focused <input>.
struct KeySpec {
  std::string key;   // DOM `key` value
  std::string code;  // DOM `code` value (physical key, layout-independent)
  int vk;            // windowsVirtualKeyCode / nativeVirtualKeyCode
  std::string text;  // empty for non-printable keys
};

// Named (non-printable) keys. VK codes are the Windows values Blink expects; the media ones are
// tree-verified against cobalt/src/starboard/key.h (kSbKeyMediaRewind = 0xE3, ..FastForward =
// 0xE4). Non-printable keys reuse `name` as their `code`.
struct NamedKey {
  std::string_view name;
  int vk;
};
constexpr NamedKey kNamedKeys[] = {
    {"ArrowUp", 38},
    {"ArrowDown", 40},
    {"ArrowLeft", 37},
    {"ArrowRight", 39},
    {"Enter", 13},
    {"Escape", 27},
    {"Backspace", 8},
    // Delete is the candidate erase key inside Leanback's OSK: `kSbKeyBack == kSbKeyBackspace ==
    // 8`, so Backspace is expected to pop the view rather than erase (findings input-ux §8.4).
    {"Delete", 46},
    {"Tab", 9},
    // Media keys. MediaRewind/MediaFastForward exist so the `scan_rewind`/`scan_forward` spike can
    // dispatch them at all; nothing binds them until that spike verifies Leanback honors them.
    {"MediaPlayPause", 179},
    {"MediaTrackNext", 176},
    {"MediaTrackPrevious", 177},
    {"MediaStop", 178},
    {"MediaRewind", 227},
    {"MediaFastForward", 228},
};

// US-layout DOM `code` + VK for the printable ASCII characters that are neither letters nor digits.
// Shifted characters carry the code/VK of the physical key that produces them ('!' is Digit1),
// which is what the DOM contract requires; the caller supplies mod::kShift.
struct PunctKey {
  char ch;
  std::string_view code;
  int vk;
};
constexpr PunctKey kPunctKeys[] = {
    {' ', "Space", 32},        {'-', "Minus", 189},        {'_', "Minus", 189},
    {'=', "Equal", 187},       {'+', "Equal", 187},        {'[', "BracketLeft", 219},
    {'{', "BracketLeft", 219}, {']', "BracketRight", 221}, {'}', "BracketRight", 221},
    {'\\', "Backslash", 220},  {'|', "Backslash", 220},    {';', "Semicolon", 186},
    {':', "Semicolon", 186},   {'\'', "Quote", 222},       {'"', "Quote", 222},
    {',', "Comma", 188},       {'<', "Comma", 188},        {'.', "Period", 190},
    {'>', "Period", 190},      {'/', "Slash", 191},        {'?', "Slash", 191},
    {'`', "Backquote", 192},   {'~', "Backquote", 192},    {'!', "Digit1", 49},
    {'@', "Digit2", 50},       {'#', "Digit3", 51},        {'$', "Digit4", 52},
    {'%', "Digit5", 53},       {'^', "Digit6", 54},        {'&', "Digit7", 55},
    {'*', "Digit8", 56},       {'(', "Digit9", 57},        {')', "Digit0", 48},
};

// `name` is either a named key ("ArrowUp", "MediaRewind") or a single printable ASCII character
// ("c", "A", "7", " "). nullopt = this client cannot synthesize it, and the caller must not guess.
std::optional<KeySpec> key_spec(std::string_view name) {
  for (const NamedKey& k : kNamedKeys)
    if (k.name == name) return KeySpec{std::string(k.name), std::string(k.name), k.vk, ""};

  if (name.size() != 1) return std::nullopt;
  const char c = name[0];
  const std::string text(1, c);
  if (c >= 'a' && c <= 'z')
    return KeySpec{text, std::string("Key") + static_cast<char>(c - 'a' + 'A'), c - 'a' + 'A',
                   text};
  if (c >= 'A' && c <= 'Z') return KeySpec{text, std::string("Key") + c, c, text};
  if (c >= '0' && c <= '9') return KeySpec{text, std::string("Digit") + c, c, text};
  for (const PunctKey& p : kPunctKeys)
    if (p.ch == c) return KeySpec{text, std::string(p.code), p.vk, text};
  return std::nullopt;
}
}  // namespace

bool DevToolsClient::key_supported(std::string_view name) { return key_spec(name).has_value(); }

bool DevToolsClient::dispatch_key(std::string_view name, int modifiers) {
  auto spec = key_spec(name);
  if (!spec) {
    warn(std::format("devtools: dispatch_key unknown key '{}'", name));
    return false;
  }
  std::string fields =
      std::format(R"("key":"{}","code":"{}","windowsVirtualKeyCode":{},"nativeVirtualKeyCode":{})",
                  json_escape(spec->key), spec->code, spec->vk, spec->vk);
  if (modifiers != mod::kNone) fields += std::format(R"(,"modifiers":{})", modifiers);
  // A printable key needs `text` on a `keyDown` (not `rawKeyDown`) for Blink to produce a char
  // event.
  const bool printable = !spec->text.empty();
  if (printable) fields += std::format(R"(,"text":"{}")", json_escape(spec->text));
  const std::string down_type = printable ? "keyDown" : "rawKeyDown";

  std::lock_guard lk(mutex_);
  bool ok =
      request("Input.dispatchKeyEvent", std::format(R"({{"type":"{}",{}}})", down_type, fields))
          .has_value();
  ok = request("Input.dispatchKeyEvent", std::format(R"({{"type":"keyUp",{}}})", fields))
           .has_value() &&
       ok;
  return ok;
}

bool DevToolsClient::dispatch_mouse(std::string_view type, double x, double y,
                                    std::string_view button, int buttons, int click_count) {
  std::lock_guard lk(mutex_);
  return request("Input.dispatchMouseEvent",
                 std::format(
                     R"({{"type":"{}","x":{},"y":{},"button":"{}","buttons":{},"clickCount":{}}})",
                     type, x, y, button, buttons, click_count))
      .has_value();
}

bool DevToolsClient::mouse_press(double x, double y) {
  return dispatch_mouse("mousePressed", x, y, "left", 1, 1);
}

bool DevToolsClient::mouse_release(double x, double y) {
  return dispatch_mouse("mouseReleased", x, y, "left", 0, 1);
}

bool DevToolsClient::mouse_click(double x, double y) {
  const bool down = mouse_press(x, y);
  return mouse_release(x, y) && down;
}

}  // namespace deckback
