#pragma once
#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "devtools.hpp"

namespace deckback {

class UpdateState;
class Updater;

// The "notify" side of self-update (findings durable/self-update.md): turn the updater's "a newer
// commit is available" signal into a painless, launcher-owned UI — a passive amber dot plus a
// one-time card — that never deploys without the user pressing "Update now", and that is reachable
// from the Menu forever. Detection and the actual deploy stay in updater.cpp; this owns only the UI
// and the dismissal state.

// ---- pure helpers (unit-tested in updateprompt_test.cpp) ----------------------------------------

// Semantic version compare on dot-separated numeric fields: "0.0.3" < "0.0.10" < "0.1.0". A leading
// 'v' is ignored. Missing fields count as 0 ("0.1" == "0.1.0"). A non-numeric field makes that side
// compare as older (malformed input never reads as "newer than a real release"). Returns <0/0/>0.
int compare_versions(std::string_view a, std::string_view b);
inline bool version_newer(std::string_view candidate, std::string_view local) {
  return compare_versions(candidate, local) > 0;
}

struct ReleaseNote {
  std::string version;  // `tag_name` with a leading 'v' stripped
  std::string title;    // release `name`
  std::string body;     // release notes (markdown text)
};

// Parse the GitHub `/releases` JSON array into notes (tag_name, name, body). Best-effort and
// tolerant of field order and unknown fields; entries without a tag are skipped. Not a full JSON
// parser — it scans the string fields it needs, like config.cpp's extractor. Pure.
std::vector<ReleaseNote> parse_github_releases(const std::string& json);

// What the card should say about the available version.
struct ChangelogView {
  std::string available_version;  // newest release strictly newer than local, or "" if none/unknown
  std::string notes;              // concatenated release notes, newest-first, clamped to max_len
};
// Normalise a GitHub release body — by contract our CHANGELOG "## [version]" section, i.e.
// Keep-a-Changelog markdown (RELEASING.md) — into clean text for the 10-foot card: "### Added"
// headings lose their hashes, "- item" / "* item" bullets unify to "• item", **bold**/`code`/__x__
// markers are dropped, and runs of blank lines collapse. This is what standardises "release notes"
// for the updater UI: whatever markdown the CHANGELOG uses, the card shows readable text. Pure.
std::string notes_to_plain(std::string_view markdown);

// Summarise the releases strictly newer than `local_version`, newest-first, notes normalised via
// notes_to_plain and clamped to `max_len` characters. Pure.
ChangelogView summarize_releases(const std::vector<ReleaseNote>& notes,
                                 std::string_view local_version, std::size_t max_len);

// Which UI to surface for the available commit `remote_commit`, given the commits already recorded
// as card-shown / dot-dismissed. show_card is the one-time auto card (first time this commit is
// seen); show_dot is the passive indicator (suppressed only if the user ignored this exact commit).
struct NotifyDecision {
  bool show_card;
  bool show_dot;
};
NotifyDecision decide_notification(std::string_view remote_commit,
                                   std::string_view card_shown_commit,
                                   std::string_view dot_dismissed_commit);

// Versioned state-file paths under the state dir (same convention as onboarding's first_run
// marker).
std::string update_card_marker_path(std::string_view state_dir);
std::string update_dot_marker_path(std::string_view state_dir);
// Read the single trimmed line ("" if absent/unreadable). Write it (a read-only dir warns, never
// crashes — mirrors onboarding::mark_first_run_done).
std::string read_update_marker(const std::string& path);
bool write_update_marker(const std::string& path, const std::string& value);

// ---- the controller (input thread owns it; one method is navigator-thread-safe) ----------------

struct UpdatePromptConfig {
  std::string cdp_host = "127.0.0.1";
  int cdp_port = 0;
  std::string state_dir;
  std::string local_version;  // compiled kDeckbackVersion, e.g. "0.0.4"
  // GitHub releases API for the changelog. Overridable so a test can point at a fixture/none.
  std::string releases_url = "https://api.github.com/repos/properrr/deckback/releases";
  UpdateState* state = nullptr;  // shared availability, written by the updater. Not owned.
  Updater* updater = nullptr;    // request_update() on confirm. Not owned.
};

class UpdatePromptController {
 public:
  explicit UpdatePromptController(UpdatePromptConfig cfg);
  ~UpdatePromptController();  // joins the changelog-fetch thread

  // Called on each input-thread tick. Cheap when nothing changed; on the first tick after a newer
  // commit becomes available it kicks the (async) changelog fetch and auto-shows the one-time card
  // once the notes are ready. Reconciles the indicator pill each tick: it shows when an
  // un-dismissed update exists AND `on_watch` is false. `on_watch` is the raw watch-view signal
  // (LayerState::video_up, i.e. player_open — NOT `layer() == Layer::Player`, which the OSK can
  // mask while a video plays underneath); while it is set, neither the pill nor the auto-card is
  // drawn, so nothing ever sits over a playing video. Never blocks the input thread on the network.
  void tick(bool on_watch);

  bool card_visible() const { return card_visible_.load(std::memory_order_acquire); }
  bool update_available() const;  // for the Menu row: is there anything to offer?

  // Card actions (input thread), each closes the card:
  void confirm_update();  // A: consent to deploy, hide dot + card, toast "Updating…"
  void dismiss_later();   // B: close, leave the dot for later
  void ignore_version();  // Y: close, hide the dot until a newer commit arrives

  // Open the card deliberately from the Menu, even if the dot was dismissed or the card was already
  // auto-shown. No-op (returns false) if no update is available.
  bool open_from_menu();

  // Called by the navigator on a full page reload (documentElement torn down). Navigator thread:
  // flags the dot for the input thread to redraw (playback-aware, so it never reappears over a
  // video), and re-injects the card if it was open — a full navigation discards the page global, so
  // otherwise the card is gone while card_visible_ stays true and input.cpp keeps swallowing keys
  // (the on-Deck input trap).
  void on_page_reloaded();

 private:
  void show_card();
  bool draw_card(DevToolsClient& client);  // inject the card DOM; no state/marker side effects
  void hide_card();
  void draw_dot();
  void hide_dot();
  // Start the one-shot background changelog fetch (idempotent): a libcurl GET of releases_url off
  // the input thread, so a slow network never freezes gamepad input. Result lands in cached_.
  void kick_changelog();
  // The fetched changelog if ready, else an empty view (the card then shows the fallback link).
  // Safe to read from the input thread: gated on the acquire-loaded fetch_state_.
  ChangelogView current_changelog() const;

  UpdatePromptConfig cfg_;
  DevToolsClient client_;    // input-thread only
  std::string last_commit_;  // last commit tick() reacted to (edge detection)
  // Indicator pill state (input-thread only). dot_desired_ = an un-dismissed update exists (set by
  // the notification decision, cleared on A/Y); dot_shown_ = the pill is currently in the DOM.
  // tick() reconciles the two against the watch-view signal.
  bool dot_desired_ = false;
  bool dot_shown_ = false;
  // Set by the navigator thread in on_page_reloaded(); tick() consumes it to know documentElement
  // was wiped (so the pill must be redrawn) without the navigator touching the input thread's DOM
  // state.
  std::atomic<bool> reloaded_{false};
  // Atomic: written by the input thread (show/hide), read by the navigator thread in
  // on_page_reloaded().
  std::atomic<bool> card_visible_{false};
  bool want_card_ = false;  // auto-card requested, waiting on the async changelog before it shows

  // Changelog fetch: 0 idle, 1 fetching, 2 done. cached_ is written by fetch_thread_ and published
  // by the release-store of fetch_state_ = 2; the input thread reads it only after an acquire-load
  // sees 2, so the two never touch cached_ concurrently.
  std::atomic<int> fetch_state_{0};
  ChangelogView cached_;
  std::thread fetch_thread_;
};

}  // namespace deckback
