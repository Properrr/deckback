#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "devtools.hpp"

namespace deckback {

// Phase 5 voice search (findings input-ux §13). Hold a button, speak, release — the TV convention.
//
// It CANNOT be a keypress. `kSbKeyMicrophone` (0x3002) exists in starboard/key.h, but Cobalt routes
// voice through the `SoftMicPlatformService` Starboard extension and S0.5 established this
// Chromium-based binary contains zero starboard symbols. Injecting that keycode does nothing, at
// any keycode. The only path is to activate Leanback's own on-screen soft-mic button, and the
// robust way is a **trusted `Input.dispatchMouseEvent`** on its `getBoundingClientRect()` centre
// rather than fragile focus-navigation. CDP mouse coords are CSS pixels, so the rect feeds straight
// in with no letterbox transform.
//
// UNVERIFIED, and it gates everything (V0): we ship a Cobalt UA, so Leanback may assume the
// platform supplies voice via h5vcc and either not render a web mic button at all, or render one
// whose handler calls a missing API. **Our own identity bet (R1) could be the thing that breaks
// voice.** So this ships default-off (`voice_enabled: false`) and, when it cannot find a button,
// says so and does nothing. A dead button is worse than a missing one: it teaches the user the
// feature is broken rather than absent.

// ---- pure helpers (no CDP) — unit-tested in tests/voice_test.cpp --------------------------------

// What to do with playback while the mic is open. Hold-to-talk during playback feeds the Deck's
// speakers into its own mics ~15 cm away; TVs duck for exactly this reason (§13.3). This likely
// decides whether server-side ASR returns anything usable, so it is not polish.
enum class DuckMode {
  None,   // leave playback alone (for the spike, and for a user who wants it off)
  Mute,   // mute the <video>, keep it playing
  Pause,  // pause the <video> — the safest for ASR, and what we default to
};

// Parse `voice_duck`. An unrecognised value returns nullopt so startup can warn rather than
// silently picking a behaviour the user did not ask for.
std::optional<DuckMode> parse_duck_mode(std::string_view value);
const char* duck_mode_name(DuckMode m);

// Parse the "<x>,<y>" reply of the mic-button probe into a viewport point. nullopt when the button
// was not found (empty string) or the reply is malformed. A zero-area rect is not a point: an
// element that exists but is not laid out cannot be clicked.
std::optional<std::pair<double, double>> parse_point(std::string_view s);

// Build the JS that locates the soft-mic button and returns its rect centre as "x,y", or "" when no
// candidate matches. Selectors come from config so a Leanback change is a config hotfix, not a
// rebuild. This is a *search*, not a guessed key binding: when nothing matches we do nothing and
// log.
std::string mic_probe_js(const std::vector<std::string>& selectors);

// A press/hold/release state machine, pure and clock-injected.
//
// `input.cpp` dispatches mapped buttons on the PRESS EDGE ONLY (`if (value != 1) return;`) and
// discards releases, so hold is impossible without this. The debounce exists so a stray tap cannot
// open the mic: the mic opens only once the button has been held for `hold_ms`.
class HoldToTalk {
 public:
  enum class Action { None, Start, Stop };

  explicit HoldToTalk(long hold_ms) : hold_ms_(hold_ms < 0 ? 0 : hold_ms) {}

  Action on_press(long now_ms);
  Action on_release(long now_ms);
  Action on_tick(long now_ms);  // call from the poll loop; fires Start once the hold matures

  bool active() const { return active_; }
  // True while a press is being held but the mic has not opened yet — the caller must keep waking
  // up until `deadline_ms()` or the hold would never mature on a pad that sends no further events.
  bool pending() const { return down_ && !active_; }
  long deadline_ms() const { return press_ms_ + hold_ms_; }

 private:
  long hold_ms_;
  bool down_ = false;
  bool active_ = false;
  long press_ms_ = 0;
};

// ---- the controller ----------------------------------------------------------------------------

struct VoiceConfig {
  bool enabled =
      false;  // default OFF until the V0 spike proves a mic button exists (input-ux §13.2)
  long hold_ms = 250;
  DuckMode duck = DuckMode::Pause;
  // If true, the page's mic control is tap-to-toggle rather than press-and-hold, so we click once
  // to open and click again to close. Unverified either way; configurable rather than guessed.
  bool click_toggles = false;
  std::vector<std::string> mic_selectors;
};

class VoiceController {
 public:
  VoiceController(std::string host, int port, VoiceConfig cfg);

  // Open the mic: duck playback, then press the soft-mic button. Returns false (and logs why) when
  // no mic button can be found — the V0 failure — leaving playback untouched.
  bool start();
  // Close the mic and restore playback. Safe to call when not started.
  void stop();

  bool listening() const { return listening_; }
  // The configured hold-to-talk debounce; the input layer's HoldToTalk is armed with this, so the
  // two cannot drift apart.
  long hold_ms() const { return cfg_.hold_ms; }

 private:
  // Why the probe did not yield a point. "Engine unreachable" and "the page has no mic button"
  // demand different actions from whoever reads the log; blaming our UA bet for a dead CDP socket
  // is the same category error the harness exit codes exist to prevent.
  struct Located {
    enum Status { Found, NoButton, Unreachable } status;
    std::pair<double, double> point;
  };
  Located locate_button();
  void duck();
  void unduck();

  DevToolsClient client_;
  VoiceConfig cfg_;
  std::string probe_js_;
  bool listening_ = false;
  bool ducked_ = false;
  std::pair<double, double> button_{0, 0};  // where we pressed, so we release in the same place
  bool warned_no_button_ = false;
};

}  // namespace deckback
