#pragma once
// A loopback fake of Cobalt's --remote-debugging-port endpoint. Serves GET /json/list, performs the
// WebSocket upgrade, and answers CDP frames. The reply shape is chosen from the expression text:
//   *  "pause()" -> a number (a stand-in currentTime)
//   *  "play()"  -> boolean true
//   *  WANT_NUM / WANT_VOID / WANT_EXC / WANT_NOISE / WANT_STR / WANT_TRAP -> explicit shapes
//   *  otherwise -> boolean == playing() (defaults true)
// Connections are served one at a time.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
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
  // The other bits of PlayerController's poll bitmask: watch view open, text field focused.
  void set_player_open(bool p) { player_open_.store(p); }
  void set_text_focused(bool p) { text_focused_.store(p); }

  // Page.navigate fails with this errorText for any URL except about:blank ("" = succeed). Models a
  // network outage in which about:blank still commits.
  void set_nav_error(std::string e) {
    std::lock_guard lk(req_mu_);
    nav_error_ = std::move(e);
  }
  // The user pressed Enter on the error page. One-shot: the launcher's poll consumes it.
  void press_retry() { retry_flag_.store(true); }
  bool error_page_up() const { return error_page_up_.load(); }
  void clear_error_page() { error_page_up_.store(false); }
  bool saw_reload() const { return saw_reload_.load(); }  // was a Page.reload request received?

  // What osd.js `op:"state"` returns: "tab=..;idx=..", "detached" (node off-DOM), or "gone" (JS
  // context wiped by a reload).
  void set_osd_state(std::string s) {
    std::lock_guard lk(req_mu_);
    osd_state_ = std::move(s);
  }
  // What osd.js `op:"cmd"` returns: a verdict ("ok", "close", "gone", "action:...").
  void set_osd_cmd_reply(std::string s) {
    std::lock_guard lk(req_mu_);
    osd_cmd_reply_ = std::move(s);
  }
  // List a service worker before the page in /json/list. `last_ws_path` is the path the client
  // upgraded on.
  void set_list_decoy(bool on) { list_decoy_.store(on); }
  std::string last_ws_path() {
    std::lock_guard lk(req_mu_);
    return last_ws_path_;
  }
  // Drop every WebSocket session after `n` text frames (0 = never): a reconnect on demand.
  void close_after_frames(int n) { close_after_.store(n); }
  // Hold the Page.navigate reply this long, as Chromium does until the navigation commits.
  void set_navigate_delay_ms(int ms) { navigate_delay_ms_.store(ms); }
  // Hold the reply to every frame naming `method` this long.
  void set_method_delay_ms(std::string method, int ms) {
    std::lock_guard lk(req_mu_);
    delay_method_ = std::move(method);
    delay_method_ms_ = ms;
  }

  // One batch for the gesture router's next drain (gestures.js `drain()` JSON); later drains read
  // an empty queue. `gestures_drained` tells a test the router has collected it.
  void queue_gestures(std::string drain_json) {
    std::lock_guard lk(req_mu_);
    gestures_ = std::move(drain_json);
  }
  bool gestures_drained() {
    std::lock_guard lk(req_mu_);
    return gestures_.empty();
  }
  // Whether the persistent Settings button is still painted (false models a document reload).
  void set_osd_button_present(bool present) { osd_button_present_.store(present); }
  // Whether an injected overlay node is still in the document (false models a reload).
  void set_overlay_present(bool present) { overlay_present_.store(present); }

  // How many WebSocket sessions and /json/list probes this server has served.
  int ws_upgrades() const { return ws_upgrades_.load(); }
  int http_gets() const { return http_gets_.load(); }

  // Drains every CDP text frame the client sent, in order.
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
      {
        std::lock_guard lk(req_mu_);
        const size_t sp = req.find(' ');
        last_ws_path_ = req.substr(sp + 1, req.find(' ', sp + 1) - sp - 1);
      }
      send_str(c,
               "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Accept: x\r\n\r\n");
      serve_ws(c);
    } else {
      http_gets_.fetch_add(1);
      const std::string ws = "ws://127.0.0.1:" + std::to_string(port_);
      std::string body = "[";
      if (list_decoy_.load())
        body += "{\"type\":\"service_worker\",\"webSocketDebuggerUrl\":\"" + ws +
                "/devtools/page/WORKER\"},";
      body += "{\"type\":\"page\",\"webSocketDebuggerUrl\":\"" + ws + "/devtools/page/DECK\"}]";
      send_str(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                      std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    }
  }

  void serve_ws(int c) {
    std::string rx;
    ws::Frame f;
    int frames = 0;
    while (recv_frame(c, rx, f)) {
      if (f.opcode == ws::kClose) return;
      if (f.opcode != ws::kText) continue;
      ++frames;
      if (const int n = close_after_.load(); n > 0 && frames > n) return;  // unanswered: dropped
      const std::string& e = f.payload;
      {
        std::lock_guard lk(req_mu_);
        reqs_.push_back(e);
      }
      if (e.find("Page.reload") != std::string::npos) saw_reload_.store(true);
      {
        int ms = 0;
        {
          std::lock_guard lk(req_mu_);
          if (!delay_method_.empty() && e.find(delay_method_) != std::string::npos)
            ms = delay_method_ms_;
        }
        if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      }
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
      // Like real CDP, a failed navigation is a successful command carrying `errorText`.
      if (e.find("Page.navigate") != std::string::npos) {
        if (const int ms = navigate_delay_ms_.load(); ms > 0)
          std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        std::string err;
        {
          std::lock_guard lk(req_mu_);
          if (e.find("about:blank") == std::string::npos) err = nav_error_;
        }
        result = err.empty() ? "\"result\":{\"frameId\":\"F\",\"loaderId\":\"L\"}"
                             : "\"result\":{\"frameId\":\"F\",\"loaderId\":\"L\",\"errorText\":\"" +
                                   err + "\"}";
      } else if (e.find("g.drain()") != std::string::npos) {
        std::string batch;
        {
          std::lock_guard lk(req_mu_);
          batch = std::exchange(gestures_, {});
        }
        if (batch.empty()) batch = R"({"configured":true,"enabled":true,"q":[]})";
        std::string quoted;
        for (char ch : batch) {
          if (ch == '"' || ch == '\\') quoted += '\\';
          quoted += ch;
        }
        result = "\"result\":{\"result\":{\"type\":\"string\",\"value\":\"" + quoted + "\"}}";
      } else if (e.find("/*osd-button-state*/") != std::string::npos) {
        result = std::string("\"result\":{\"result\":{\"type\":\"boolean\",\"value\":") +
                 (osd_button_present_.load() ? "true" : "false") + "}}";
      } else if (e.find("/*overlay-state*/") != std::string::npos) {
        result = std::string("\"result\":{\"result\":{\"type\":\"boolean\",\"value\":") +
                 (overlay_present_.load() ? "true" : "false") + "}}";
      } else if (e.find("\\\"op\\\":\\\"state\\\"") != std::string::npos) {
        // The osd.js arg arrives JSON-escaped inside the CDP frame.
        std::lock_guard lk(req_mu_);
        result = "\"result\":{\"result\":{\"type\":\"string\",\"value\":\"" + osd_state_ + "\"}}";
      } else if (e.find("\\\"op\\\":\\\"cmd\\\"") != std::string::npos) {
        std::lock_guard lk(req_mu_);
        result =
            "\"result\":{\"result\":{\"type\":\"string\",\"value\":\"" + osd_cmd_reply_ + "\"}}";
      } else if (e.find("\\\"op\\\":\\\"open\\\"") != std::string::npos)
        result = "\"result\":{\"result\":{\"type\":\"string\",\"value\":\"ok\"}}";
      else if (e.find("\\\"op\\\":\\\"close\\\"") != std::string::npos)
        result = "\"result\":{\"result\":{\"type\":\"string\",\"value\":\"closed\"}}";
      // A thrown expression nests `exceptionDetails` inside `result`, as real CDP does.
      else if (e.find("WANT_EXC") != std::string::npos)
        result =
            "\"result\":{\"result\":{\"type\":\"object\",\"subtype\":\"error\"},"
            "\"exceptionDetails\":{\"text\":\"boom\"}}";
      // The error page's retry flag: a one-shot, read and cleared by the same expression.
      else if (e.find("window.__deckbackRetry=false;return r;") != std::string::npos)
        result = std::string("\"result\":{\"result\":{\"type\":\"boolean\",\"value\":") +
                 (retry_flag_.exchange(false) ? "true" : "false") + "}}";
      // Is our error page the loaded document?
      else if (e.find("__deckback_error") != std::string::npos &&
               e.find("getElementById") != std::string::npos &&
               e.find("innerHTML") == std::string::npos)
        result = std::string("\"result\":{\"result\":{\"type\":\"boolean\",\"value\":") +
                 (error_page_up_.load() ? "true" : "false") + "}}";
      // The error page injection.
      else if (e.find("documentElement.innerHTML") != std::string::npos) {
        error_page_up_.store(true);
        result = "\"result\":{\"result\":{\"type\":\"boolean\",\"value\":true}}";
      }
      // PlayerController's poll (the only expression using `activeElement`): a bitmask number.
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
        // A URL with \/ escapes.
        result =
            "\"result\":{\"result\":{\"type\":\"string\","
            "\"value\":\"https:\\/\\/www.youtube.com\\/tv#\\/\"}}";
      // A page string that merely contains the failure markers; not a CDP error.
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
  std::string osd_cmd_reply_ = "ok";               // guarded by req_mu_
  std::string delay_method_;                       // guarded by req_mu_
  int delay_method_ms_ = 0;                        // guarded by req_mu_
  std::string gestures_;                           // guarded by req_mu_
  std::string last_ws_path_;                       // guarded by req_mu_
  std::atomic<bool> list_decoy_{false};
  std::atomic<int> close_after_{0};
  std::atomic<int> navigate_delay_ms_{0};
  std::atomic<bool> saw_reload_{false};
  std::mutex req_mu_;
  std::vector<std::string> reqs_;
  std::thread thread_;
};

}  // namespace deckback::testing
