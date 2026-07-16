#include "onboarding.hpp"

#include <cstdio>
#include <filesystem>
#include <format>

#include "keymap.hpp"  // resolve_binding, parse_chord
#include "log.hpp"
#include "scripts.hpp"

namespace deckback {
namespace {

struct Label {
  std::string_view id;
  std::string_view text;
};

// What is actually printed on a Steam Deck. "View"/"Menu" are Valve's names for the two small
// buttons; users do not call them SELECT and START, and the glyphs are what they can see.
constexpr Label kControlLabels[] = {
    {"dpad", "D-pad"},      {"a", "A"},   {"b", "B"},   {"x", "X"},   {"y", "Y"},
    {"lb", "L1"},           {"rb", "R1"}, {"lt", "L2"}, {"rt", "R2"}, {"start", "Menu (☰)"},
    {"select", "View (⧉)"}, {"l3", "L3"}, {"r3", "R3"},
};

// Semantic action -> user-facing phrase. Only the actions the launcher can actually perform appear
// here; the rest fall through to `resolve_binding`, which decides whether the control does anything
// at all.
constexpr Label kActionLabels[] = {
    {"select", "Select"},
    {"back", "Back"},
    {"playpause", "Play / pause"},
    {"toggle_captions", "Captions"},
    {"scrub_back", "Scrub back"},
    {"scrub_fwd", "Scrub forward"},
    {"seek_back_10", "Scrub back"},  // deprecated aliases still resolve, so they still print
    {"seek_fwd_10", "Scrub forward"},
    {"chapter_back", "Previous chapter"},
    {"chapter_fwd", "Next chapter"},
    {"skip_back", "Skip back"},
    {"skip_fwd", "Skip forward"},
    {"voice_search", "Hold to speak"},
};

// Actions the launcher performs itself over CDP, with no DOM key: voice_search and the chapter/skip
// seeks (LT/RT). `show_controls` is a retired compatibility action; Menu is fixed to Settings and
// is appended below, independent of the keymap.
bool is_launcher_action(std::string_view value) {
  return value == "voice_search" || skip_action_sign(value) != 0 || chapter_action_sign(value) != 0;
}

}  // namespace

std::string control_label(std::string_view name) {
  for (const Label& l : kControlLabels)
    if (l.id == name) return std::string(l.text);
  return {};
}

std::string action_label(std::string_view value) {
  for (const Label& l : kActionLabels)
    if (l.id == value) return std::string(l.text);
  // A raw DOM key from a hot-swapped config. Print it verbatim rather than invent a phrase for it:
  // the user who wrote `"a": "MediaTrackNext"` knows what that means, and we do not.
  return std::string(value);
}

std::vector<ControlRow> controls_overlay_rows(const OverlayContext& ctx) {
  std::vector<ControlRow> rows;

  for (const auto& [name, value] : ctx.keymap) {
    // The D-pad is not a button binding; it is described below alongside the sticks. Only the name
    // is checked: a `"navigate"` *value* on some other control already resolves to no key and is
    // dropped by the guard further down, so testing for it here would be a check that cannot fail.
    if (name == "dpad" || name == "start" || value == "show_controls") continue;
    const std::string control = control_label(name);
    if (control.empty()) continue;  // a control this hardware does not have

    // Voice is a launcher action, and it is off by default. Advertising "Hold to speak" on a build
    // where the mic button was never found is the dead-button failure the voice work exists to
    // avoid (input-ux §13.2).
    if (value == "voice_search" && !ctx.voice_enabled) continue;

    // Anything that dispatches no key and is not a launcher action does nothing at all. Listing it
    // would teach the user a control that silently fails — worse than not mentioning it.
    if (!is_launcher_action(value) && resolve_binding(value).empty()) continue;

    rows.push_back({control, action_label(value)});
  }

  // Controls the launcher owns rather than the keymap.
  rows.push_back({"Menu (☰)", "Settings"});
  rows.push_back({"D-pad / Left stick", "Move focus"});
  if (ctx.right_stick_scroll) rows.push_back({"Right stick", "Fast scroll"});

  if (ctx.touch_lock_enabled) {
    const auto [a, b] = parse_chord(ctx.touch_lock_chord);
    if (a >= 0) {
      // Print the chord the config actually names, uppercased ("l3+r3" -> "L3 + R3"). A card that
      // says L3+R3 while the config binds Menu+View is a card that lies.
      std::string pretty;
      for (char c : ctx.touch_lock_chord) {
        if (c == '+')
          pretty += " + ";
        else
          pretty += static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
      }
      rows.push_back({pretty, "Lock touchscreen (hold to unlock)"});
    }
  }
  return rows;
}

std::string overlay_js(const std::vector<ControlRow>& rows, std::string_view title,
                       std::string_view footer) {
  // The card markup, style, and Trusted Types policy live in config/scripts/overlay.js
  // (ScriptLibrary). Rows are passed as structured [control, action] pairs, escaped ONCE by
  // ScriptParams — the page builds the <td>s. Trusted Types is required: youtube.com/tv's CSP
  // rejects a bare innerHTML, which is why the card rendered in tests but nothing on the Deck until
  // the policy was added (input-ux §17). Appended (not innerHTML-replaced): Leanback is alive under
  // it.
  std::vector<std::pair<std::string, std::string>> pairs;
  pairs.reserve(rows.size());
  for (const ControlRow& r : rows) pairs.emplace_back(r.control, r.action);
  return ScriptLibrary::instance().render(
      "overlay", ScriptParams().set("title", title).set("footer", footer).set("rows", pairs));
}

std::string overlay_hide_js() { return ScriptLibrary::instance().render("overlay_hide"); }

std::string first_run_marker_path(std::string_view state_dir) {
  // Versioned: when the controls change materially, bump the suffix and every existing user sees
  // the card once more. A single unversioned marker would silently hide a changed keymap forever.
  return std::string(state_dir) + "/first_run_v1";
}

bool first_run_pending(const std::string& marker_path) {
  std::error_code ec;
  return !std::filesystem::exists(marker_path, ec);
}

bool mark_first_run_done(const std::string& marker_path) {
  std::error_code ec;
  std::filesystem::path p(marker_path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::FILE* f = std::fopen(marker_path.c_str(), "w");
  if (!f) {
    // A read-only state dir must not mean the card reappears on every single launch — but it also
    // must not crash. Warn once and move on; the user sees it again next boot, which is the mild
    // failure of the two.
    warn(std::format("onboarding: cannot write {} — the controls card will show again",
                     marker_path));
    return false;
  }
  std::fputs("1\n", f);
  std::fclose(f);
  return true;
}

OnboardingController::OnboardingController(std::string host, int port, OverlayContext ctx,
                                           std::string marker_path)
    : client_(std::move(host), port), ctx_(std::move(ctx)), marker_path_(std::move(marker_path)) {}

bool OnboardingController::show(bool first_run_only) {
  if (first_run_only && !first_run_pending(marker_path_)) return false;
  if (visible()) return false;

  const auto rows = controls_overlay_rows(ctx_);
  if (rows.empty()) return false;  // nothing to teach

  if (!client_.eval_void(overlay_js(rows, "Controls", "Press any button to continue"))) {
    warn("onboarding: could not draw the controls card (engine unreachable)");
    return false;
  }
  state_.set(true);
  // Recorded on *show*, not on dismiss: if the app is killed while the card is up, the user has
  // still seen it, and a card that reappears until it is politely dismissed is a card that
  // reappears after every crash.
  if (first_run_only) mark_first_run_done(marker_path_);
  info(std::format("onboarding: controls card shown ({} rows)", rows.size()));
  return true;
}

void OnboardingController::hide() {
  if (!state_.set(false)) return;  // already hidden; do not spend a CDP round trip per button press
  client_.eval_void(overlay_hide_js());
  info("onboarding: controls card dismissed");
}

}  // namespace deckback
