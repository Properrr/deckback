#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace deckback {

// The sleep timer (TASKS P12.6): "stop playing in N minutes" for the handheld case nothing else
// covers — falling asleep watching, with the Deck playing video until the battery is flat.
//
// What it does on expiry is deliberately small: it PAUSES the video and nothing else. That is
// enough, because pausing drops PlayState.playing, which makes PlayerController release the logind
// "playback active" idle inhibitor, which is what the host idle-nudge helper is gated on
// (findings durable/keep-awake.md) — so gamescope's idle timer finally fires and SteamOS dims and
// auto-suspends by itself. There is deliberately NO login1.Suspend call: it is unnecessary given
// that chain, and every new sandbox permission strands existing users on self-update
// (durable/feature-landscape.md §1.1). tests/deck/test_sleep.py asserts the chain from outside.
//
// Timing uses CLOCK_BOOTTIME, so a countdown that elapses while the Deck is asleep is expired on
// resume rather than silently restarted.
//
// Single-threaded, exactly like CaptionSettings: only the launcher's input thread touches it —
// GamepadInput's loop ticks it and OsdMenuController::exec edits it, and both run there.

// One entry of the OSD combo the timer renders. Same field names the captions rows use, so osd.js
// draws both with one widget.
struct SleepOption {
  std::string value;  // minutes as text; "0" is Off
  std::string label;  // "Off", "15 min", "1 h 30 min"
};

// The shipped ladder, in minutes. Off is implicit and always first, so it is not listed here.
// Fractional entries are allowed and are what makes the on-Deck fire test able to run in bounded
// time against a hot-swapped app.json.
std::vector<double> default_sleep_options();

// Parse `sleep_timer_options_minutes` — a comma-separated minute list, e.g. "5, 15, 30". Junk
// entries and non-positive values are dropped; an empty result means "use the default ladder", so a
// bad hot-swap degrades to the shipped behaviour instead of a menu with nothing in it.
std::vector<double> parse_sleep_options(std::string_view csv);

// "15 min", "1 h", "1 h 30 min", "90 s". Used for both the ladder labels and the countdown, so a
// duration never reads one way when you pick it and another way while it runs.
std::string format_sleep_minutes(double minutes);
std::string format_remaining(long ms);

class SleepTimer {
 public:
  // `warn_seconds` is the lead time of the "about to stop" toast; 0 disables the warning. The
  // warning is also suppressed for any duration not longer than the lead, where it would fire at
  // the same moment it was armed and tell the user nothing.
  SleepTimer(std::vector<double> options_minutes, int warn_seconds);

  bool armed() const { return armed_; }
  double selected_minutes() const { return minutes_; }  // 0 when off
  // The configured lead, so the warning can say how long is actually left instead of hard-coding
  // "a minute" and lying the moment someone hot-swaps the value.
  int warn_seconds() const { return warn_seconds_; }

  // Arm for `minutes` (<= 0 disarms) as of `now_ms`, a boottime_ms() reading. Re-arming resets both
  // the warn and the fire edges, so changing your mind never skips the warning.
  void arm(double minutes, long now_ms);
  void disarm();

  // What this tick owes the user. Warn and Fire are each returned at most once per arming; Fire
  // also disarms, so a paused video is never re-paused every second afterwards.
  enum class Tick { None, Warn, Fire };
  Tick tick(long now_ms);

  long remaining_ms(long now_ms) const;  // 0 when not armed

  // The OSD status line: "Off" or "Stops in 4 min". Computed here rather than in the page, so the
  // menu cannot show a countdown the launcher is not actually running.
  std::string status_line(long now_ms) const;

  // The whole OSD model as a compact JSON object {ns:"sleep",status:"..",rows:[..]}, handed to
  // osd.js verbatim (ScriptParams::set_raw) the same way the captions model is.
  std::string osd_model_json(long now_ms) const;

  // The ladder as the combo's options, Off first.
  std::vector<SleepOption> options() const;

  // Apply a "sleep.*" action from the OSD ("sleep.timer=15"). True when something changed.
  bool apply_action(std::string_view action, long now_ms);

 private:
  std::vector<double> options_;
  int warn_seconds_;
  bool armed_ = false;
  double minutes_ = 0;
  long deadline_ms_ = 0;
  bool warned_ = false;
};

}  // namespace deckback
