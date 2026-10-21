#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "devtools.hpp"
#include "updater.hpp"  // UpdateProgress, UpdateState

namespace deckback {

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

// Parse the GitHub `/releases` JSON array into notes (tag_name, name, body). Goes through the real
// parser (json.hpp), so escapes and \uXXXX decode correctly and a "name"/"body" cannot bleed across
// release objects. Entries without a tag are skipped; unparseable input warns and yields nothing
// (the card then shows the releases link). Pure.
std::vector<ReleaseNote> parse_github_releases(const std::string& text);

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

// Notes for exactly `version` — "what's new in the build you are running". summarize_releases only
// keeps releases NEWER than local, so without this the Updates tab has nothing to show whenever no
// update is pending, which is almost always. Empty when that version has no release entry. Pure.
std::string release_notes_for(const std::vector<ReleaseNote>& notes, std::string_view version,
                              std::size_t max_len);

// ---- the published-ref probe (TASKS U4)
// ----------------------------------------------------------
//
// "Check for updates" used to call the portal's Update(), which checks by INSTALLING: it skipped
// notify mode's consent step, and the portal's own detection is a shared 0–30 min timer that never
// runs on a metered network. Instead the launcher reads the 65-byte ref file the published repo
// serves for this app (refs/heads/app/<id>/x86_64/master) and compares it with the commit it is
// running. The portal stays the only installer and the only signature verifier: a forged or stale
// ref can at worst announce an update the portal then answers Empty for.

// The commit a ref body names: exactly 64 lowercase hex digits once surrounding whitespace is
// trimmed, or "" for anything else (an HTML error page, a truncated body). Pure.
std::string parse_ref_commit(std::string_view ref_body);

// UpdateAvailable means "differs", not "newer": the ref is what flatpak would move to, and a recall
// is a different commit too. Unknown covers an unreadable body or an unknown running commit. Pure.
enum class RefCheck { UpdateAvailable, UpToDate, Unknown };
RefCheck decide_ref_check(std::string_view ref_body, std::string_view running_commit);

// Where a requested deploy has got to. Checking is the ref probe a "Check for updates" started:
// nothing is installed by it. Requested is a portal Update() — "Update now", or a check whose probe
// could not read the repo and fell back to the portal, which then answers Empty when there was
// nothing to do. PermissionBlocked is split out from Failed because it is the one failure the user
// cannot retry their way out of — see is_permission_change_failure.
enum class DeployPhase { Idle, Checking, Requested, Done, Empty, Failed, PermissionBlocked };

// "Updating… step 2 of 3 (45%)" from a portal Progress (op is 0-based). Pure.
std::string deploy_progress_status(std::uint32_t op, std::uint32_t n_ops, std::uint32_t percent);

// Everything the Updates tab shows, derived in one place so the status text and the buttons can
// never disagree — the v0.0.7 report was exactly that disagreement: a "will apply after restart"
// message sitting above a deploy that had already been refused.
struct UpdatesView {
  std::string status;
  bool has_update = false;  // offer Update now + Ignore this version
  bool can_check = false;   // offer Check for updates
  bool can_quit = false;    // offer Close now (a staged update binds on the next launch)
};
// A deploy verdict outranks availability: it is the more recent, more specific thing that happened.
// `updater_present` is false when self-update is off, which is the one case with nothing to offer.
// `progress_status` (deploy_progress_status) replaces the generic Requested text once the portal
// reports progress. Pure.
UpdatesView updates_view(DeployPhase phase, std::string_view error_message, bool update_available,
                         std::string_view available_version, std::string_view local_version,
                         std::string_view idle_status, bool updater_present,
                         std::string_view progress_status = {});

// The toast for a terminal deploy phase, or "" for the non-terminal ones. Only used when the OSD is
// closed — an open Updates panel already shows the same thing, patched in place. Pure.
std::string deploy_toast(DeployPhase phase);

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
  // What the tab says before anything has happened — the configured policy, from main.cpp. Never a
  // claim that the installed build is current: UpdateMonitor only ever reports positive
  // availability, so silence is "nothing announced", not "you are up to date".
  std::string idle_status;
  // GitHub releases API for the changelog. Overridable so a test can point at a fixture/none.
  std::string releases_url = "https://api.github.com/repos/properrr/deckback/releases";
  UpdateState* state = nullptr;      // shared availability, written by the updater. Not owned.
  Updater* updater = nullptr;        // request_update() on confirm. Not owned.
  OsdMenuController* osd = nullptr;  // the Updates-tab UI + button badge. Not owned.
  // How long "Updating…" may wait for the portal's verdict before reading as a failure. A real
  // deploy of the ~77 MB engine finishes in seconds to minutes; silence past this means a hung
  // transaction or an updater that is not running, and the tab would otherwise never move on. Every
  // progress report restarts it, so a slow runtime pull is not cut off while it is still moving.
  long deploy_timeout_ms = 10L * 60 * 1000;
  // The published-ref probe (U4). Both set: the launch probes once and "Check for updates" asks the
  // repo directly, installing nothing. Either missing (outside the sandbox there is no running
  // commit; a build without libcurl cannot fetch): Check falls back to the portal's Update().
  // `fetch_remote_ref` returns the ref body or nullopt, and runs on the probe thread.
  std::string running_commit{};
  std::function<std::optional<std::string>()> fetch_remote_ref{};
  // How long a commit the portal answered Empty for stays un-announced by the probe. It matches the
  // CDN's max-age on `summary`: the likely cause is a fresh ref ahead of a cached summary, and past
  // this the portal can install it, so a check should offer it again.
  long refusal_window_ms = 10L * 60 * 1000;
};

class UpdatePromptController {
 public:
  explicit UpdatePromptController(UpdatePromptConfig cfg);
  ~UpdatePromptController();  // joins the changelog-fetch and probe threads

  // Each input-thread tick: feed the OSD the current update state (status + changelog), kicking the
  // async changelog fetch on the first tick a newer un-ignored commit appears. Never blocks on the
  // network. `on_watch` is accepted for signature symmetry; the OSD button owns playback gating.
  void tick(bool on_watch);

  bool update_available() const;

  // OSD action callbacks (input thread).
  void confirm_update();  // consent to deploy; clears the badge until a newer commit
  void ignore_version();  // hide the badge until a newer commit (persisted marker)
  // Read the published ref and say what it found: "latest", or an update to confirm. Installs
  // nothing (U4). Without a configured probe it falls back to the portal's Update(), which checks
  // by deploying and answers Empty when there is nothing newer.
  void check_now();

  // Terminal deploy verdict, called on the UPDATER's thread. Only records it; the input thread
  // renders it on the next tick.
  void on_deploy_result(UpdateProgress progress, const std::string& error_name,
                        const std::string& error_message);
  // A Running deploy Progress, called on the UPDATER's thread (UpdaterConfig::on_running).
  void on_deploy_progress(std::uint32_t op, std::uint32_t n_ops, std::uint32_t percent);

  DeployPhase deploy_phase() const;  // for diagnostics and tests

 private:
  void kick_changelog();
  bool fetch_done() const;
  ChangelogView current_changelog() const;
  void feed(const UpdatesView& view, const std::string& notes);
  void request_deploy(const char* why);
  bool probe_configured() const;
  void kick_probe();
  void consume_probe();

  UpdatePromptConfig cfg_;
  DevToolsClient client_;      // input-thread only (the confirm toast)
  std::string last_commit_;    // last commit tick() reacted to (edge detection)
  bool notify_ = false;        // an un-ignored update is available
  std::string hidden_status_;  // empty -> the generic "no update" idle status (kNoUpdateStatus)

  // Last values fed to the OSD, so tick() only calls set_update_model on a change.
  bool fed_has_ = false;
  bool fed_check_ = false;
  bool fed_quit_ = false;
  bool fed_valid_ = false;
  std::string fed_status_, fed_notes_;

  // Deploy phase, written by the updater thread and read by the input thread. `fresh_` makes the
  // toast fire exactly once per verdict rather than on every tick that observes it.
  mutable std::mutex deploy_mu_;
  DeployPhase deploy_phase_ = DeployPhase::Idle;
  std::string deploy_error_;
  bool deploy_fresh_ = false;
  long requested_at_ms_ = 0;     // when the pending deploy was requested or last reported progress
  std::string deploy_progress_;  // deploy_progress_status of the latest Running report

  // Changelog fetch: 0 idle, 1 fetching, 2 done. cached_ is published by the release-store of
  // fetch_state_ = 2; the input thread reads it only after an acquire-load sees 2.
  std::atomic<int> fetch_state_{0};
  ChangelogView cached_;
  std::string installed_notes_;  // release notes for the RUNNING version ("what's new"), same guard
  std::thread fetch_thread_;

  // Ref probe: 0 idle, 1 running, 2 done. probe_result_/probe_commit_ are published by the
  // release-store of 2 and read by the input thread after an acquire-load sees it, exactly like
  // cached_. Everything else here is input-thread only.
  std::atomic<int> probe_state_{0};
  RefCheck probe_result_ = RefCheck::Unknown;
  std::string probe_commit_;  // short remote commit from the probe
  std::thread probe_thread_;
  bool launch_probed_ = false;   // the one probe a launch makes has been started
  bool probe_for_user_ = false;  // the running probe answers a "Check for updates" press
  // The probe's last announcement, and a commit the portal then answered Empty for. Only the probe
  // is filtered by it: a ref the portal cannot install from (a stale CDN copy of the summary, an
  // install from another remote) must not be re-announced on every check.
  std::string probe_announced_;
  std::string probe_refused_;
  long probe_refused_at_ms_ = 0;
  bool refused(const std::string& commit) const;
};

}  // namespace deckback
