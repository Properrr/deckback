#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "devtools.hpp"

namespace deckback {

// Owns the TV-UA-injection + initial-navigation contract over CDP (findings m114 S0.2).
// content_shell ignores `--user-agent` and, if handed the app URL positionally, would load it once
// with the default Chrome UA before any override could land — YouTube then redirects to the desktop
// site. So the engine is booted on about:blank and this Navigator, on a background thread:
//   * installs the sticky TV UA override (DevToolsClient re-arms it across the target teardown that
//     Leanback triggers on navigation), then
//   * navigates to the app URL, and re-navigates whenever it finds the engine off the TV app —
//   which
//     covers a watchdog restart (fresh about:blank engine) or an accidental desktop redirect.
// Uses its own DevToolsClient; content_shell (Chromium M114) allows multiple concurrent CDP clients
// per page target, so this runs alongside PlayerController's client without contention.
// Startup CDP policy applied once the UA is armed and before the first navigation (Phase 4/5).
struct NavPolicy {
  bool steer_av1 = true;      // inject the AV1->unsupported codec-steering script (findings S0.5)
  bool mic_autogrant = true;  // Browser.grantPermissions(audioCapture) for the app origin

  // Failure-state UX (findings input-ux §7). On a failed navigation, replace Chromium's
  // controller-hostile error interstitial with our own focusable Retry page and keep retrying with
  // backoff. `error_title`/`error_hint` override the page's text — empty means "derive from the
  // net:: error", and a non-empty pair is the R1 hotfix surface: a config push, not a rebuild.
  bool error_page = true;
  long retry_min_ms = 2000;
  long retry_max_ms = 30000;
  std::string error_title;
  std::string error_hint;

  bool disable_pointer = false;  // inject no_pointer.js to swallow touch-as-mouse events (Option A)
};

class Navigator {
 public:
  Navigator(std::string host, int port, std::string user_agent, std::string url, int poll_ms,
            NavPolicy policy = {});
  ~Navigator();

  Navigator(const Navigator&) = delete;
  Navigator& operator=(const Navigator&) = delete;

  // Fires on the navigator thread each time the TV app becomes loaded (including after a watchdog
  // restart). The first-run controls card hangs off this: there is no point drawing an overlay onto
  // a document that has not arrived. Set before start().
  void set_on_app_loaded(std::function<void()> cb) { on_app_loaded_ = std::move(cb); }

  void start();  // launch the navigate/keepalive thread
  void stop();   // signal + join (idempotent; also called by the destructor)

 private:
  void loop();
  bool wait_or_stop(int ms);  // returns true if asked to stop
  // One navigation attempt. Enters (or leaves) the error state depending on what CDP reports, and
  // schedules the next automatic retry. Returns whether we landed on the app.
  bool try_navigate();

  DevToolsClient client_;
  std::string user_agent_;
  std::string url_;
  int poll_ms_;
  NavPolicy policy_;
  std::function<void()> on_app_loaded_;

  // Error state. `showing_error_` means our Retry page is (or should be) on screen; it is the
  // reason the loop must not judge "are we on the app?" by the location alone — a failed navigation
  // leaves `document.location.href` set to the URL we asked for.
  bool showing_error_ = false;
  int fail_attempt_ = 0;
  long next_retry_ms_ = 0;

  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  bool started_ = false;
};

}  // namespace deckback
