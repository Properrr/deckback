#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "json.hpp"

namespace deckback {

// Minimal Chrome DevTools Protocol (CDP) client for Cobalt's --remote-debugging-port: raw POSIX
// sockets and a hand-rolled RFC 6455 WebSocket client. The endpoint is loopback on a port we
// launched, so Sec-WebSocket-Accept is not verified and the target list is trusted.
//
// A DevToolsClient is a cheap handle. All clients for the same host:port share one CdpSession (one
// socket, one reader thread demultiplexing replies by CDP id), so requests from different threads
// pipeline and a wait never blocks another caller's send. Sticky state (UA override,
// on-new-document scripts, permission grants) lives on the session and is re-armed on reconnect.

// RFC 6455 frame codec — pure, no I/O, exposed for unit testing.
namespace ws {

struct Frame {
  bool fin = true;
  uint8_t opcode = 0;  // 0x1 text, 0x2 binary, 0x8 close, 0x9 ping, 0xA pong
  std::string payload;
};

constexpr uint8_t kText = 0x1;
constexpr uint8_t kClose = 0x8;
constexpr uint8_t kPing = 0x9;
constexpr uint8_t kPong = 0xA;

// Encode one frame. Client→server frames MUST be masked (masked=true); server frames must not be.
std::string encode_frame(uint8_t opcode, std::string_view payload, bool masked, uint32_t mask,
                         bool fin = true);

struct DecodeResult {
  enum Status { Ok, NeedMore, Error } status = NeedMore;
  Frame frame;
  size_t consumed = 0;
};

// Decode the first frame in `buf`. NeedMore = buffer truncated mid-frame (read more, retry). Error
// = protocol violation / oversized payload (drop the connection).
DecodeResult decode_frame(std::string_view buf);

}  // namespace ws

// CDP `Input.dispatchKeyEvent` modifier bitfield (protocol-defined values, not ours to choose).
namespace mod {
constexpr int kNone = 0;
constexpr int kAlt = 1;
constexpr int kCtrl = 2;
constexpr int kMeta = 4;
constexpr int kShift = 8;
}  // namespace mod

// The shared, demultiplexed connection behind every DevToolsClient for one endpoint.
class CdpSession;

class DevToolsClient {
 public:
  DevToolsClient(std::string host, int port);
  ~DevToolsClient();

  DevToolsClient(const DevToolsClient&) = delete;
  DevToolsClient& operator=(const DevToolsClient&) = delete;

  // True once a WebSocket session to a page target is established.
  bool connected();

  // Runtime.evaluate the expression and return its primitive result. nullopt on any transport or
  // CDP error (the connection is dropped and lazily re-established on the next call).
  std::optional<bool> eval_bool(std::string_view expression);
  std::optional<double> eval_number(std::string_view expression);
  // Like eval_bool but for a string result (e.g. `document.location.href`); nullopt if the value is
  // absent/non-string or on any error. The returned string is JSON-unescaped.
  std::optional<std::string> eval_string(std::string_view expression);
  // eval_string for an expression that may return a Promise: the reply waits for it to settle, for
  // up to `timeout_ms` (a long-poll).
  std::optional<std::string> eval_string_await(std::string_view expression, long timeout_ms);

  // Evaluate for side effects (e.g. `video.play()`); returns true if the engine accepted the call.
  bool eval_void(std::string_view expression);

  // Install a sticky user-agent override (Network.setUserAgentOverride); content_shell ignores the
  // `--user-agent` flag. Re-armed on every reconnect, since Leanback tears the page target down on
  // navigation and a fresh target reverts to the default UA. Returns engine acceptance.
  bool set_user_agent_override(std::string_view ua);

  // Page.navigate to `url`. Returns whether the engine accepted the command.
  bool navigate(std::string_view url);

  // The outcome of one `Page.navigate`. `sent` is false on a transport failure. `error_text` is
  // CDP's `errorText` (e.g. "net::ERR_NAME_NOT_RESOLVED"), empty when the navigation committed.
  // Use this, not `document.location.href`: a failed navigation still sets the location.
  struct NavStatus {
    bool sent = false;
    std::string error_text;
    bool ok() const { return sent && error_text.empty(); }
  };
  NavStatus navigate_checked(std::string_view url);

  // Page.reload ignoring the HTTP cache. Returns engine acceptance.
  bool reload();

  // Install a script that runs at the start of every new document, before the page's own scripts
  // (Page.addScriptToEvaluateOnNewDocument). Sticky: re-installed on every reconnect. `source` is
  // raw JS (JSON-escaped internally). Returns engine acceptance.
  bool add_script_on_new_document(std::string_view source);

  // Grant one permission (e.g. "audioCapture") to `origin` via Browser.grantPermissions. Sticky:
  // re-applied on reconnect. Returns engine acceptance.
  bool grant_permissions(std::string_view origin, std::string_view permission);

  // Synthesize a trusted (isTrusted=true) key press via Input.dispatchKeyEvent. `name` is a DOM key
  // value accepted by `key_supported()`; `modifiers` is the CDP bitfield (`mod::`). Returns engine
  // acceptance.
  bool dispatch_key(std::string_view name, int modifiers = 0);

  // Whether `name` is a DOM key this client can synthesize: a named navigation/media key, or any
  // single printable ASCII character (dispatched with `text` so Blink produces a char event).
  static bool key_supported(std::string_view name);

 private:
  // Runtime.evaluate `expression` and return the reply's `result.result.value`. With
  // `timeout_ms` > 0 a returned Promise is awaited and the request may take that long.
  std::optional<json::Value> eval_value(std::string_view expression, long timeout_ms = 0);

  // Never null; shared by every client for the same host:port.
  std::shared_ptr<CdpSession> session_;
};

}  // namespace deckback
