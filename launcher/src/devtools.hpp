#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace deckback {

// Minimal Chrome DevTools Protocol (CDP) client for talking to Cobalt's --remote-debugging-port.
// Dependency-free: raw POSIX sockets + a hand-rolled RFC 6455 WebSocket client (we do NOT rely on
// libcurl's experimental WS API). Used by the Phase 6 power hooks to pause/checkpoint the player on
// suspend, nudge it on resume, and to read play-state for the idle inhibitor (doc §2 P6).
//
// Trust model: the endpoint is loopback on a port we launched, so the client skips
// Sec-WebSocket-Accept verification (no crypto dep) and trusts the target list.
//
// ---- one socket, many clients -------------------------------------------------------------------
//
// A DevToolsClient is a cheap HANDLE, not a connection. Every client for the same host:port shares
// one CdpSession: one TCP socket, one WebSocket, one reader thread that demultiplexes replies by
// CDP id. Constructing seven of them (player, navigator, gamepad, onboarding, osd, update_prompt,
// updateprompt's toast) opens one connection, not seven.
//
// This used to be seven independent sockets, and that was a rational workaround rather than an
// oversight: request() held a mutex across the ENTIRE round trip (up to 2.5 s), so a shared client
// would have let the navigator's poll stall gamepad key injection. The fan-out bought isolation by
// paying for it elsewhere:
//
//   * discover_ws_path() takes the FIRST target in /json/list, and each client discovered
//     independently at a different time. Nothing pinned them to the same target, so after a
//     Leanback teardown the navigator's UA could be installed on one target while the gamepad
//     dispatched keys into another.
//   * The sticky TV UA's lifetime was tied to the NAVIGATOR's socket. If that session dropped,
//     Chromium reverted the override and it stayed reverted until the navigator's next 1 Hz poll
//     re-armed it -- any load committing in that window used content_shell's default UA and got the
//     desktop redirect (R1).
//   * Each connect cost two TCP connects (discover, then dial): 14 for seven clients.
//
// Demultiplexing removes the reason for the workaround AND is strictly more concurrent than the
// fan-out was: the send lock is held only for the write, never across the wait, so requests from
// different threads pipeline instead of serializing. Sticky state (UA, on-new-document scripts,
// permission grants) lives on the session, so it is installed once and re-armed on any reconnect
// rather than only on the poll tick of whichever client happened to own it.

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

// The shared, demultiplexed connection behind every DevToolsClient. Defined in devtools.cpp: no
// caller needs its shape, only that handles to the same endpoint share one.
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

  // Evaluate for side effects (e.g. `video.play()`); returns true if the engine accepted the call.
  bool eval_void(std::string_view expression);

  // Install a sticky user-agent override (Network.setUserAgentOverride). content_shell ignores the
  // `--user-agent` launch flag, so this is how the TV UA reaches YouTube (findings m114 S0.2). The
  // UA is remembered and re-armed automatically on every reconnect, because Leanback tears the CDP
  // page target down on navigation and a fresh target would otherwise revert to the default UA.
  // Returns whether the override was accepted by the engine.
  bool set_user_agent_override(std::string_view ua);

  // Page.navigate to `url`. Returns whether the engine accepted the command.
  bool navigate(std::string_view url);

  // The outcome of one `Page.navigate`. `sent` is false when the engine could not be reached at all
  // (a transport failure, not a navigation failure). `error_text` carries CDP's `errorText` — e.g.
  // "net::ERR_NAME_NOT_RESOLVED" — and is empty when the navigation committed.
  //
  // This distinction is load-bearing. A failed navigation still leaves `document.location.href` set
  // to the URL we asked for, so anything that judges "did we land on the app?" by inspecting the
  // location will conclude *yes* while the user stares at Chromium's error interstitial.
  struct NavStatus {
    bool sent = false;
    std::string error_text;
    bool ok() const { return sent && error_text.empty(); }
  };
  NavStatus navigate_checked(std::string_view url);

  // Page.reload (ignoring the HTTP cache) — reloads the current document so Leanback re-fetches
  // fresh stream URLs and refreshes its auth token (Phase 6 resume recovery). Returns engine
  // acceptance.
  bool reload();

  // Install a script that runs at the start of every new document, before the page's own scripts
  // (Page.addScriptToEvaluateOnNewDocument). Used to steer YouTube's codec probing away from AV1
  // (Phase 4). Sticky: re-installed automatically on every reconnect, because Leanback tears the
  // page target down on navigation and a fresh target would otherwise drop the override. `source`
  // is raw JS (JSON-escaped internally). Returns whether the engine accepted the install.
  bool add_script_on_new_document(std::string_view source);

  // Grant a single permission (e.g. "audioCapture") to `origin` via Browser.grantPermissions, so
  // the TV app's mic (voice search) is auto-approved without a prompt for that origin only (Phase
  // 5). Sticky: re-applied on reconnect. Returns whether the engine accepted the grant.
  bool grant_permissions(std::string_view origin, std::string_view permission);

  // Synthesize a trusted key press (rawKeyDown/keyDown + keyUp) via Input.dispatchKeyEvent. `name`
  // is a DOM key value — see `key_supported()` for the set. CDP-injected keys are trusted
  // (isTrusted=true), so Leanback's handlers fire (S0.6). `modifiers` is the CDP bitfield (see
  // `mod::`); it is what makes a `Shift+…` binding expressible at all. Returns engine acceptance.
  bool dispatch_key(std::string_view name, int modifiers = 0);

  // Whether `name` is a DOM key this client can synthesize: one of the named navigation/media keys,
  // or any single printable ASCII character (which dispatches with `text` so Blink produces a real
  // char event — the path text entry into Leanback's search field needs). Lets the input layer
  // validate a config-supplied keymap at startup instead of failing silently on every button press.
  static bool key_supported(std::string_view name);

  // Trusted synthetic mouse events (Input.dispatchMouseEvent). Coordinates are **CSS pixels**, so
  // an element's getBoundingClientRect() feeds straight in with no letterbox transform — unlike raw
  // touch evdev coordinates. This is how voice search is activated: `kSbKeyMicrophone` does nothing
  // on this build (Cobalt routes voice through a Starboard service Chrobalt lacks — S0.5), so the
  // only path is a trusted click on Leanback's own soft-mic button (findings input-ux §13.2).
  // press/release are exposed separately so hold-to-talk can hold the button down.
  bool mouse_press(double x, double y);
  bool mouse_release(double x, double y);
  bool mouse_click(double x, double y);  // press then release at the same point

 private:
  bool dispatch_mouse(std::string_view type, double x, double y, std::string_view button,
                      int buttons, int click_count);
  // Runtime.evaluate `expression` and return the raw `"value":` token of the reply — the shared
  // front half of every eval_*() method.
  std::optional<std::string> eval_token(std::string_view expression);

  // The shared connection. Never null. Clients for the same host:port hold the same session, so
  // this is where the socket, the reader thread and the sticky state actually live.
  std::shared_ptr<CdpSession> session_;
};

}  // namespace deckback
