#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "devtools.hpp"
#include "worker.hpp"

namespace deckback {

// Touch gesture router. config/scripts/touch_gestures.js recognises gestures in the page and queues
// them; this long-polls that queue over CDP (the page answers as soon as a gesture is queued) and
// turns each one into a trusted key event or a player call. A page cannot synthesize a trusted key
// and the launcher cannot read the touch panel, hence the split; DevToolsClient drops CDP events,
// hence the long-poll. `poll_ms` is the retry interval while the page has no router to wait on.

// The DOM key a gesture maps to, or "" when it is not a key gesture (seek and hold are not).
std::string gesture_key(std::string_view kind, std::string_view dir);

// +1 forward / -1 back for a seek gesture; 0 for anything else.
int gesture_seek_sign(std::string_view kind, int dir);

// Is this a hold (press-and-hold scrub), and is it the press or the release?
// Returns 1 for press, -1 for release, 0 when the gesture is not a hold.
int gesture_hold_phase(std::string_view kind, bool on);

// The playback rate a held zone asks for: the left third gets `slow`, the right and the centre get
// `fast`. (Chromium has no negative playbackRate, so there is no hold-to-rewind.)
double hold_rate_for_zone(std::string_view zone, double fast, double slow);

// What the on-screen seek indicator says. `seconds` is the accumulated total for the burst, not the
// increment.
std::string seek_hud_text(int seconds);
// Rate label for a held scrub, e.g. "2x" or "0.5x".
std::string hold_hud_text(double rate);

// One decoded gesture from the page's queue. Unknown kinds are preserved so the router can log and
// ignore them (a page script newer than the launcher is supported).
struct Gesture {
  std::string kind;
  std::string dir;
  std::string zone;  // hold: which third the finger went down in -> the scrub direction
  int seek_dir = 0;
  int n = 1;  // seek: how many double-taps this run has accumulated (mobile's 10/20/30/40)
  bool on = false;
};

// Decode the payload of `__deckbackGestures.drain()`. Returns false when the JSON is unusable --
// which is not the same as an empty queue, and the caller must not treat it as one.
struct GestureBatch {
  bool ok = false;
  bool configured = false;  // false after a page reload installed a fresh copy with defaults
  bool enabled = true;
  std::vector<Gesture> gestures;
};
GestureBatch parse_drain(std::string_view json_text);

struct GestureRouterConfig {
  std::string cdp_host = "127.0.0.1";
  int cdp_port = 0;
  int poll_ms = 50;
  int step_px = 70;
  int max_steps = 1;
  int edge_px = 40;
  int long_press_ms = 550;
  int double_tap_ms = 280;
  int skip_seconds = 10;
  // Rates the right third (and the centre) / the left third ask for while held. hold_scrub.js
  // snaps each to the nearest rate the player advertises.
  double hold_rate = 2.0;
  double hold_slow_rate = 0.5;
  // True while a modal surface (the OSD, the first-run card) owns input. Polled per drain from the
  // router's thread, so it must only read atomics. Null = never modal.
  std::function<bool()> modal;
};

class GestureRouter {
 public:
  explicit GestureRouter(GestureRouterConfig cfg);
  ~GestureRouter();

  GestureRouter(const GestureRouter&) = delete;
  GestureRouter& operator=(const GestureRouter&) = delete;

  void start();
  void stop();

  // Enable/disable the router. Returns the new state. Safe to call from the input thread while the
  // poll thread runs.
  bool set_enabled(bool on);
  bool enabled() const { return enabled_.load(std::memory_order_acquire); }
  bool toggle() { return set_enabled(!enabled()); }

 private:
  void loop();
  void configure_page(DevToolsClient& client);
  // The on-screen indicator (config/scripts/gesture_hud.js). `ms` 0 pins it until it is hidden with
  // an empty text -- what a press-and-hold needs.
  void show_hud(DevToolsClient& client, std::string_view text, std::string_view side, int ms);
  void act(DevToolsClient& client, const Gesture& g);

  GestureRouterConfig cfg_;
  std::atomic<bool> enabled_{true};
  // Set by set_enabled() from another thread; the poll thread pushes it to the page on its next
  // tick, keeping the CDP round trip off the caller's thread.
  std::atomic<bool> push_enabled_{false};
  WorkerThread worker_;
};

}  // namespace deckback
