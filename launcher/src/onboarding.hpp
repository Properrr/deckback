#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "devtools.hpp"
#include "pageoverlay.hpp"

namespace deckback {

// First-run controls overlay (findings input-ux §7; doc P3).
//
// This app's controls are unguessable from Leanback's own UI — `View (⧉)` toggles captions and
// Menu opens Settings, neither of which YouTube advertises — and `docs/SUPPORT.md` and
// `install.sh` are invisible from Game Mode. Every console app and every Deck-Verified title shows
// a one-shot controls card for exactly this reason.
//
// The overlay is **derived from the live keymap, never hardcoded.** `config/app.json` is designed
// to be hot-swapped remotely (doc §6 R1), so a card that says "View = Captions" while the config
// rebound View would be teaching the user something false — and a controls card nobody can trust is
// worse than none, because they will stop reading it. For the same reason a control that resolves
// to
// **no** key is omitted rather than listed: showing a binding that does nothing is the dead-button
// failure again (see voice, input-ux §13).

// ---- pure helpers (no CDP) — unit-tested in tests/onboarding_test.cpp
// ----------------------------

struct ControlRow {
  std::string control;  // "A", "View (⧉)", "L3 + R3" — what is printed on the hardware
  std::string action;   // "Select", "Captions" — what it does, in the user's words
};

// Everything the card needs to know about the running configuration. Sourced from the same values
// the input layer resolves, so the two cannot disagree.
struct OverlayContext {
  std::vector<std::pair<std::string, std::string>> keymap;  // app.json `keymap`, in written order
  bool right_stick_scroll = true;
};

// The rows to print. Order follows `keymap` as written, then the controls the launcher owns rather
// than the keymap (Menu, the sticks). Controls that dispatch nothing are dropped.
std::vector<ControlRow> controls_overlay_rows(const OverlayContext& ctx);

// Deck control id ("lb", "select") -> the label printed on the hardware ("L1", "View (⧉)"). Returns
// "" for a name no Deck control has.
std::string control_label(std::string_view name);

// A semantic action or DOM key -> human text ("playpause" -> "Play / pause"). A raw DOM key that we
// have no phrase for is returned as-is: a user who hot-swapped `"a": "MediaTrackNext"` is better
// served by seeing `MediaTrackNext` than by seeing nothing.
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

// ---- the controller ----------------------------------------------------------------------------

class OnboardingController {
 public:
  OnboardingController(std::string host, int port, OverlayContext ctx, std::string marker_path);

  // Draw the card. `first_run_only` shows it once ever, and records that it did; otherwise it is
  // the user re-opening the card deliberately and no marker is touched.
  bool show(bool first_run_only);
  void hide();

  // Is the card on screen? Read by the input thread on every event: while it is up the card is
  // modal, so this gates swallowing the event and pinning the D-pad.
  bool visible() const { return card_.painted(); }

  // Per input-tick health check. The card is drawn into Leanback's document, and a reload deletes
  // it without telling us — which used to leave `visible()` true forever after, eating the user's
  // next button press to dismiss a card that was no longer there and freezing the D-pad until they
  // pressed one. Throttled; a no-op when the card is down.
  void tick();

 private:
  DevToolsClient client_;
  OverlayContext ctx_;
  std::string marker_path_;
  PageOverlay card_;
};

}  // namespace deckback
