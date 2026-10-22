#pragma once
#include <functional>
#include <string>
#include <string_view>

#include "devtools.hpp"
#include "worker.hpp"

namespace deckback {

// The "already on the app" needle for `url`: host+path without scheme, query/fragment, or trailing
// slashes (https://www.youtube.com/tv -> www.youtube.com/tv). A loaded ".../tv#/" contains it; a
// desktop redirect or about:blank does not.
std::string app_needle(std::string_view url);

// Startup CDP policy, applied once the UA is armed and before the first navigation.
struct NavPolicy {
  bool steer_av1 = true;      // inject the AV1->unsupported codec-steering script
  bool mic_autogrant = true;  // Browser.grantPermissions(audioCapture) for the app origin

  // On a failed navigation, show our focusable Retry page instead of Chromium's interstitial and
  // retry with backoff. Empty `error_title`/`error_hint` mean "derive from the net:: error".
  bool error_page = true;
  long retry_min_ms = 2000;
  long retry_max_ms = 30000;
  std::string error_title;
  std::string error_hint;

  bool disable_pointer = false;  // inject no_pointer.js to swallow touch-as-mouse events

  // Inject touch_gestures.js, the gesture router. MUTUALLY EXCLUSIVE with disable_pointer:
  // no_pointer.js's capture listener stops propagation, so the router would receive nothing.
  // main() enforces the exclusion.
  bool touch_gestures = false;

  // Inject hide_cursor.js; independent of the router.
  bool hide_cursor = false;
};

// Owns TV-UA injection and navigation over CDP on a background thread. The engine boots on
// about:blank (content_shell ignores `--user-agent`, so a positional URL would load with the
// default UA and be redirected to desktop). The Navigator arms the sticky TV UA, navigates to the
// app, and re-navigates whenever it finds the engine off the app (watchdog restart, redirect).
class Navigator {
 public:
  Navigator(std::string host, int port, std::string user_agent, std::string url, int poll_ms,
            NavPolicy policy = {});
  ~Navigator();

  Navigator(const Navigator&) = delete;
  Navigator& operator=(const Navigator&) = delete;

  // Fires on the navigator thread each time the TV app becomes loaded (including after a watchdog
  // restart). Set before start().
  void set_on_app_loaded(std::function<void()> cb) { on_app_loaded_ = std::move(cb); }

  void start();  // launch the navigate/keepalive thread
  void stop();   // signal + join (idempotent; also called by the destructor)

 private:
  void loop();
  // One navigation attempt: enters or leaves the error state and schedules the next retry.
  // Returns whether we landed on the app.
  bool try_navigate();

  DevToolsClient client_;
  std::string user_agent_;
  std::string url_;
  int poll_ms_;
  NavPolicy policy_;
  std::function<void()> on_app_loaded_;

  // Our Retry page is (or should be) on screen. While set, the location cannot tell whether we are
  // on the app: a failed navigation still sets `document.location.href`.
  bool showing_error_ = false;
  int fail_attempt_ = 0;
  long next_retry_ms_ = 0;

  WorkerThread worker_;
};

}  // namespace deckback
