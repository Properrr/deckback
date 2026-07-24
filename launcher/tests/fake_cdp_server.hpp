#pragma once
// A loopback fake of Cobalt's --remote-debugging-port endpoint, shared by the devtools and player
// tests. Serves GET /json/list for target discovery, performs the WebSocket upgrade, and answers
// Runtime.evaluate frames. Reply shape is chosen from markers or from the real expression text:
//   *  "pause()" in the expression  -> a number (a stand-in currentTime), for the suspend
//   checkpoint
//   *  "play()"  in the expression  -> boolean true, for the resume nudge
//   *  WANT_NUM / WANT_VOID / WANT_EXC / WANT_NOISE -> explicit shapes for the codec-level tests
//   *  otherwise -> boolean == playing() (defaults true), the is-playing poll answer
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "devtools.hpp"

namespace deckback::testing {

inline std::string recv_until(int fd, const std::string& marker) {
  std::string buf;
  char tmp[1024];
  while (buf.find(marker) == std::string::npos) {
    ssize_t n = recv(fd, tmp, sizeof tmp, 0);
    if (n <= 0) break;
    buf.append(tmp, static_cast<size_t>(n));
  }
  return buf;
}

inline bool recv_frame(int fd, std::string& rx, ws::Frame& out) {
  for (;;) {
    ws::DecodeResult dr = ws::decode_frame(rx);
    if (dr.status == ws::DecodeResult::Ok) {
      out = dr.frame;
      rx.erase(0, dr.consumed);
      return true;
    }
    if (dr.status == ws::DecodeResult::Error) return false;
    char tmp[1024];
    ssize_t n = recv(fd, tmp, sizeof tmp, 0);
    if (n <= 0) return false;
    rx.append(tmp, static_cast<size_t>(n));
  }
}

inline void send_str(int fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
    if (n <= 0) return;
    off += static_cast<size_t>(n);
  }
}

class FakeServer {
 public:
  FakeServer() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    assert(listen_fd_ >= 0);
    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // ephemeral
    assert(bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    assert(listen(listen_fd_, 4) == 0);
    socklen_t len = sizeof addr;
    getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { serve(); });
  }

  ~FakeServer() {
    stop_.store(true);
    shutdown(listen_fd_, SHUT_RDWR);
    close(listen_fd_);
    if (thread_.joinable()) thread_.join();
  }

  int port() const { return port_; }
  void set_playing(bool p) { playing_.store(p); }
  // The other two signals PlayerController's poll bitmask carries (watch view up / text field
  // focused), so a test can drive the input-layer switch without a real Leanback.
  void set_player_open(bool p) { player_open_.store(p); }
  void set_text_focused(bool p) { text_focused_.store(p); }
  // Whether the page exposes a soft-mic button (the V0 question). Default: it does.

  // Page.navigate fails with this errorText for any URL except about:blank. "" = navigations
  // succeed. Models a network outage: `about:blank` still commits, so the error page can be drawn.
  void set_nav_error(std::string e) {
    std::lock_guard lk(req_mu_);
    nav_error_ = std::move(e);
  }
  // The user pressed Enter on the error page. One-shot: the launcher's poll consumes it.
  void press_retry() { retry_flag_.store(true); }
  bool error_page_up() const { return error_page_up_.load(); }
  void clear_error_page() { error_page_up_.store(false); }
  bool saw_reload() const { return saw_reload_.load(); }  // was a Page.reload request received?

  // What osd.js `op:"state"` returns: a live "tab=..;idx=.." string, "detached" (a keep-alive'd
  // node momentarily off-DOM), or "gone" (JS context wiped by a full reload). Drives the OSD
  // reconciler.
  void set_osd_state(std::string s) {
    std::lock_guard lk(req_mu_);
    osd_state_ = std::move(s);
  }
  // Whether the persistent Settings button is still painted. Lets the OSD unit test model a
  // same-URL document reload: the old local `button_shown_` flag survives, the DOM does not.
  void set_osd_button_present(bool present) { osd_button_present_.store(present); }
  // Model a Leanback reload deleting an injected overlay node out from under us.
  void set_overlay_present(bool present) { overlay_present_.store(present); }

  // How many WebSocket sessions and /json/list probes this server has served. Every DevToolsClient
  // for one host:port shares a single CdpSession, so N clients must produce exactly ONE of each --
  // this is the assertion behind that claim. Note this fake serves connections SERIALLY, so the
  // seven-independent-sockets design could not even have passed the test below.
  int ws_upgrades() const { return ws_upgrades_.load(); }
  int http_gets() const { return http_gets_.load(); }

  // Every CDP text frame the client sent, in order. Lets a test assert on the *wire format* of an
  // Input.dispatchKeyEvent rather than on our own return value (TEST-PLAN §0: never assert on a
  // value we chose). `take_requests` drains, so each test starts from a clean slate.
  std::vector<std::string> take_requests() {
    std::lock_guard lk(req_mu_);
    return std::exchange(reqs_, {});
  }

 private:
  void serve() {
    while (!stop_.load()) {
      int c = accept(listen_fd_, nullptr, nullptr);
      if (c < 0) return;
      handle(c);
      close(c);
    }
  }

  void handle(int c) {
    std::string req = recv_until(c, "\r\n\r\n");
    if (req.find("Upgrade: websocket") != std::string::npos) {
      ws_upgrades_.fetch_add(1);
      send_str(c,
               "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Accept: x\r\n\r\n");
      serve_ws(c);
    } else {
      http_gets_.fetch_add(1);
      const std::string body = std::string("[{\"type\":\"page\",\"webSocketDebuggerUrl\":\"ws://") +
                               "127.0.0.1:" + std::to_string(port_) + "/devtools/page/DECK\"}]";
      send_str(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                      std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    }
  }

  void serve_ws(int c) {
    std::string rx;
    ws::Frame f;
    while (recv_frame(c, rx, f)) {
      if (f.opcode == ws::kClose) return;
      if (f.opcode != ws::kText) continue;
      const std::string& e = f.payload;
      {
        std::lock_guard lk(req_mu_);
        reqs_.push_back(e);
      }
      if (e.find("Page.reload") != std::string::npos) saw_reload_.store(true);
      if (e.find("WANT_NOISE") != std::string::npos) {
        send_str(c, ws::encode_frame(
                        ws::kText,
                        "{\"method\":\"Runtime.consoleAPICalled\",\"params\":{\"type\":\"log\"}}",
                        false, 0));
        send_str(c,
                 ws::encode_frame(
                     ws::kText,
                     "{\"id\":9999,\"result\":{\"result\":{\"type\":\"boolean\",\"value\":false}}}",
                     false, 0));
      }
      size_t k = e.find("\"id\":");
      std::string id =
          k == std::string::npos ? "0" : e.substr(k + 5, e.find_first_of(",}", k) - (k + 5));

      std::string result;
      // Page.navigate. Real CDP reports a failed navigation as a *successful* command carrying an
      // `errorText` field — never as a CDP error — which is exactly the trap the navigator has to
      // avoid. about:blank always commits, even when the network is down, so the error page itself
      // can always be drawn.
      if (e.find("Page.navigate") != std::string::npos) {
        std::string err;
        {
          std::lock_guard lk(req_mu_);
          if (e.find("about:blank") == std::string::npos) err = nav_error_;
        }
        result = err.empty() ? "\"result\":{\"frameId\":\"F\",\"loaderId\":\"L\"}"
                             : "\"result\":{\"frameId\":\"F\",\"loaderId\":\"L\",\"errorText\":\"" +
                                   err + "\"}";
      } else if (e.find("/*osd-button-state*/") != std::string::npos) {
        result = std::string("\"result\":{\"result\":{\"type\":\"boolean\",\"value\":") +
                 (osd_button_present_.load() ? "true" : "false") + "}}";
        // PageOverlay's "is my node still in the document?" probe. Settable so a test can model the
        // thing that actually happens on a Deck: Leanback reloads and silently deletes it.
      } else if (e.find("/*overlay-state*/") != std::string::npos) {
        result = std::string("\"result\":{\"result\":{\"type\":\"boolean\",\"value\":") +
                 (overlay_present_.load() ? "true" : "false") + "}}";
      } else if (e.find("\\\"op\\\":\\\"state\\\"") != std::string::npos) {
        // The osd.js expression is JSON-escaped into the CDP frame, so the {"op":"state"} arg
        // arrives as \"op\":\"state\". Match that, and reply with whatever paint state the test
        // set.
        std::lock_guard lk(req_mu_);
        result = "\"result\":{\"result\":{\"type\":\"string\",\"value\":\"" + osd_state_ + "\"}}";
      } else if (e.find("\\\"op\\\":\\\"open\\\"") != std::string::npos)
        result = "\"result\":{\"result\":{\"type\":\"string\",\"value\":\"ok\"}}";
      else if (e.find("\\\"op\\\":\\\"close\\\"") != std::string::npos)
        result = "\"result\":{\"result\":{\"type\":\"string\",\"value\":\"closed\"}}";
      // The REAL CDP shape: a thrown expression nests `exceptionDetails` inside `result`, alongside
      // the RemoteObject. This used to emit it at the top level — which no Chromium ever sends, and
      // which only "worked" against a client that searched the whole payload for the key rather
      // than reading its documented path. A fake that models the wrong shape cannot test the
      // client that reads the right one.
      else if (e.find("WANT_EXC") != std::string::npos)
        result =
            "\"result\":{\"result\":{\"type\":\"object\",\"subtype\":\"error\"},"
            "\"exceptionDetails\":{\"text\":\"boom\"}}";
      // The error page's retry flag. Modelled as a real one-shot: the expression both reads and
      // clears `window.__deckbackRetry`, so a launcher that forgets to consume it would retry
      // forever off a single keypress — and only a stateful fake can catch that.
      else if (e.find("window.__deckbackRetry=false;return r;") != std::string::npos)
        result = std::string("\"result\":{\"result\":{\"type\":\"boolean\",\"value\":") +
                 (retry_flag_.exchange(false) ? "true" : "false") + "}}";
      // Is our error page the loaded document?
      else if (e.find("__deckback_error") != std::string::npos &&
               e.find("getElementById") != std::string::npos &&
               e.find("innerHTML") == std::string::npos)
        result = std::string("\"result\":{\"result\":{\"type\":\"boolean\",\"value\":") +
                 (error_page_up_.load() ? "true" : "false") + "}}";
      // The error page injection itself. Records that the page went up.
      else if (e.find("documentElement.innerHTML") != std::string::npos) {
        error_page_up_.store(true);
        result = "\"result\":{\"result\":{\"type\":\"boolean\",\"value\":true}}";
      }
      // PlayerController's poll: one expression, three signals, returned as a bitmask number.
      // Keyed on `activeElement`, which appears only in that expression.
      else if (e.find("activeElement") != std::string::npos)
        result = "\"result\":{\"result\":{\"type\":\"number\",\"value\":" +
                 std::to_string((playing_.load() ? 1 : 0) | (player_open_.load() ? 2 : 0) |
                                (text_focused_.load() ? 4 : 0)) +
                 "}}";
      else if (e.find("WANT_NUM") != std::string::npos)
        result = "\"result\":{\"result\":{\"type\":\"number\",\"value\":42.5}}";
      else if (e.find("WANT_VOID") != std::string::npos)
        result = "\"result\":{\"result\":{\"type\":\"undefined\"}}";
      else if (e.find("WANT_STR") != std::string::npos)
        // A JSON-escaped URL (\/ escapes) to exercise the string reader's unescaping.
        result =
            "\"result\":{\"result\":{\"type\":\"string\","
            "\"value\":\"https:\\/\\/www.youtube.com\\/tv#\\/\"}}";
      // A page string that merely CONTAINS the failure markers. Nothing here is a CDP error, so the
      // client must return the string. Reading `error` / `exceptionDetails` by path rather than by
      // searching the payload is what makes that possible.
      else if (e.find("WANT_TRAP") != std::string::npos)
        result =
            "\"result\":{\"result\":{\"type\":\"string\","
            "\"value\":\"{\\\"error\\\":1} exceptionDetails\"}}";
      else if (e.find("pause()") != std::string::npos)
        result = "\"result\":{\"result\":{\"type\":\"number\",\"value\":123.5}}";
      else if (e.find("play()") != std::string::npos)
        result = "\"result\":{\"result\":{\"type\":\"boolean\",\"value\":true}}";
      else
        result = std::string("\"result\":{\"result\":{\"type\":\"boolean\",\"value\":") +
                 (playing_.load() ? "true" : "false") + "}}";
      send_str(c, ws::encode_frame(ws::kText, "{\"id\":" + id + "," + result + "}", false, 0));
    }
  }

  int listen_fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<bool> playing_{true};
  std::atomic<bool> player_open_{false};
  std::atomic<bool> text_focused_{false};
  std::atomic<bool> retry_flag_{false};
  std::atomic<int> ws_upgrades_{0};
  std::atomic<int> http_gets_{0};
  std::atomic<bool> error_page_up_{false};
  std::atomic<bool> osd_button_present_{true};
  std::atomic<bool> overlay_present_{true};
  std::string nav_error_;                          // guarded by req_mu_
  std::string osd_state_ = "tab=settings;idx=-1";  // guarded by req_mu_
  std::atomic<bool> saw_reload_{false};
  std::mutex req_mu_;
  std::vector<std::string> reqs_;
  std::thread thread_;
};

}  // namespace deckback::testing
