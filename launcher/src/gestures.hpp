#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

#include "devtools.hpp"
#include "worker.hpp"

namespace deckback {

// Touch gesture router — the launcher half of P12.4 (findings durable/touch-gestures.md).
//
// config/scripts/touch_gestures.js recognises gestures in the page and queues them; this drains
// that queue over CDP and turns each one into a TRUSTED key event or a player call. The split is
// forced, not stylistic: a page cannot synthesize a trusted key (m114.md), and the launcher cannot
// see touch (the seat user cannot open the panel node — touch-lock.md). Polling rather than
// subscribing is likewise forced: DevToolsClient drops CDP events, there is no demux
// (devtools.cpp), so there is no channel for the page to push on.
//
// The whole touch input latency budget is one poll interval. Recognition in the page is immediate;
// `poll_ms` is how long a recognised gesture waits to become a keypress.

// ---- pure mapping (unit-tested in gestures_test.cpp) ----

// The DOM key a gesture maps to, or "" when it is not a key gesture (seek and hold are not).
// Kept as a free function so the mapping can be checked without an engine, a page or a Deck.
std::string gesture_key(std::string_view kind, std::string_view dir);

// +1 forward / -1 back for a seek gesture; 0 for anything else.
int gesture_seek_sign(std::string_view kind, int dir);

// Is this a hold (press-and-hold scrub), and is it the press or the release?
// Returns 1 for press, -1 for release, 0 when the gesture is not a hold.
int gesture_hold_phase(std::string_view kind, bool on);

// The playback rate a held zone asks for: the LEFT third slows down, the right and the centre speed
// up. Same thirds the double-tap seek uses, so one spatial model covers both gestures.
//
// Both directions are a rate, deliberately. Making the left third a rewind would have meant stepped
// seeks on a timer -- Chromium has no negative playbackRate -- i.e. a second mechanism with its own
// feel and its own failure modes, to express something the rate ladder already covers.
double hold_rate_for_zone(std::string_view zone, double fast, double slow);

// What the on-screen indicator says. `seconds` is the ACCUMULATED jump, so the user reads the total
// they are about to take rather than the increment -- which is the whole point of accumulating.
std::string seek_hud_text(int seconds);
// Rate label for a held scrub, e.g. "2x" or "0.5x". Trailing zeros read badly at 34px.
std::string hold_hud_text(double rate);

// One decoded gesture from the page's queue. Unknown kinds are preserved rather than dropped so the
// router can log them: a page script newer than the launcher is a supported state (scripts are
// hot-swappable) and must degrade to "ignored", never to a misfire.
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

// ---- router ----

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
  // Rate the RIGHT third (and the centre) asks for while held. 2.0 is mobile's hold-for-2x. The
  // player advertises [0.25 .. 2] (P12.0b) and hold_scrub.js snaps to the nearest advertised value.
  double hold_rate = 2.0;
  // ...and what the LEFT third asks for. 0.5 is half speed; 0.25 is the slowest the player offers.
  double hold_slow_rate = 0.5;
};

class GestureRouter {
 public:
  explicit GestureRouter(GestureRouterConfig cfg);
  ~GestureRouter();

  GestureRouter(const GestureRouter&) = delete;
  GestureRouter& operator=(const GestureRouter&) = delete;

  void start();
  void stop();

  // The hardware toggle (input-ux §4: the lock is never silent). Returns the new state. Safe to
  // call from the input thread while the poll thread runs.
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
  // tick. Doing the CDP call on the caller's thread would put a network round trip in the middle of
  // an evdev handler.
  std::atomic<bool> push_enabled_{false};
  WorkerThread worker_;
};

}  // namespace deckback
