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
class OsdMenuController;

// The "notify" side of self-update (findings durable/self-update.md, osd-menu-plan.md §11).
// Detection (updater.cpp) publishes availability into UpdateState; this turns it into the OSD
// Updates model (status + changelog) via OsdMenuController::set_update_model, and owns the
// deploy-consent + ignore actions the OSD calls back into. The UI itself is the OSD; this no longer
// draws anything.

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
  UpdateState* state = nullptr;      // shared availability, written by the updater. Not owned.
  Updater* updater = nullptr;        // request_update() on confirm. Not owned.
  OsdMenuController* osd = nullptr;  // the Updates-tab UI + button badge. Not owned.
};

class UpdatePromptController {
 public:
  explicit UpdatePromptController(UpdatePromptConfig cfg);
  ~UpdatePromptController();  // joins the changelog-fetch thread

  // Each input-thread tick: feed the OSD the current update state (status + changelog), kicking the
  // async changelog fetch on the first tick a newer un-ignored commit appears. Never blocks on the
  // network. `on_watch` is accepted for signature symmetry; the OSD button owns playback gating.
  void tick(bool on_watch);

  bool update_available() const;

  // OSD action callbacks (input thread).
  void confirm_update();  // consent to deploy + toast; clears the badge until a newer commit
  void ignore_version();  // hide the badge until a newer commit (persisted marker)

 private:
  void kick_changelog();
  ChangelogView current_changelog() const;
  void feed(bool has_update, const std::string& status, const std::string& notes);

  UpdatePromptConfig cfg_;
  DevToolsClient client_;      // input-thread only (the confirm toast)
  std::string last_commit_;    // last commit tick() reacted to (edge detection)
  bool notify_ = false;        // an un-ignored update is available
  std::string hidden_status_;  // empty -> the generic "no update" idle status (kNoUpdateStatus)

  // Last values fed to the OSD, so tick() only calls set_update_model on a change.
  bool fed_has_ = false;
  bool fed_valid_ = false;
  std::string fed_status_, fed_notes_;

  // Changelog fetch: 0 idle, 1 fetching, 2 done. cached_ is published by the release-store of
  // fetch_state_ = 2; the input thread reads it only after an acquire-load sees 2.
  std::atomic<int> fetch_state_{0};
  ChangelogView cached_;
  std::thread fetch_thread_;
};

}  // namespace deckback
