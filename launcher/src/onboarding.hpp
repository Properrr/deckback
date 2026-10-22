#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "devtools.hpp"
#include "pageoverlay.hpp"

namespace deckback {

// First-run controls overlay. The rows are derived from the live keymap, never hardcoded; a control
// that resolves to no key is omitted.

struct ControlRow {
  std::string control;  // "A", "View (⧉)", "L3 + R3" — what is printed on the hardware
  std::string action;   // "Select", "Captions" — what it does, in the user's words
};

// Everything the card needs to know about the running configuration, sourced from the same values
// the input layer resolves.
struct OverlayContext {
  std::vector<std::pair<std::string, std::string>> keymap;  // app.json `keymap`, in written order
  bool right_stick_scroll = true;
  // Whether the touch gesture router is actually running (the resolved state, not the raw config
  // flag). The gesture rows are listed only then.
  bool touch_gestures = false;
};

// The rows to print. Order follows `keymap` as written, then the controls the launcher owns rather
// than the keymap (Menu, the sticks). Controls that dispatch nothing are dropped.
std::vector<ControlRow> controls_overlay_rows(const OverlayContext& ctx);

// Deck control id ("lb", "select") -> the label printed on the hardware ("L1", "View (⧉)"). Returns
// "" for a name no Deck control has.
std::string control_label(std::string_view name);

// A semantic action or DOM key -> human text ("playpause" -> "Play / pause"). A raw DOM key with no
// phrase is returned as-is.
std::string action_label(std::string_view value);

// The DOM id config/scripts/overlay.js assigns to the card. Also the handle PageOverlay probes to
// notice the page removing it, so the two must not drift.
inline constexpr const char* kOverlayElementId = "__deckback_help";

// The JS that draws the modal card, and the JS that removes it. Pure, so the escaping is testable.
std::string overlay_js(const std::vector<ControlRow>& rows, std::string_view title,
                       std::string_view footer);
std::string overlay_hide_js();

// Where the "already shown" marker lives, and the two operations on it. Versioned in the filename:
// when the controls change materially, bump it and every user sees the card once more.
std::string first_run_marker_path(std::string_view state_dir);
bool first_run_pending(const std::string& marker_path);
bool mark_first_run_done(const std::string& marker_path);

class OnboardingController {
 public:
  OnboardingController(std::string host, int port, OverlayContext ctx, std::string marker_path);

  // Draw the card. `first_run_only` shows it once ever, and records that it did; otherwise it is
  // the user re-opening the card deliberately and no marker is touched.
  bool show(bool first_run_only);
  void hide();

  // Is the card on screen? Read by the input thread on every event: while it is up the card is
  // modal.
  bool visible() const { return card_.painted(); }

  // Per input-tick health check: notices a page reload deleting the card, so `visible()` turns
  // false. Throttled; a no-op when the card is down.
  void tick();

 private:
  DevToolsClient client_;
  OverlayContext ctx_;
  std::string marker_path_;
  PageOverlay card_;
};

}  // namespace deckback
