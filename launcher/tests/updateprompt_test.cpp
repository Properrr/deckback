// Self-update notify UI: version comparison, release parsing and changelog summary, the card/dot
// decision, dismissal markers, the Updates tab view, and the published-ref probe.
#include "updateprompt.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "harness.hpp"
#include "osdmenu.hpp"

using namespace deckback;

static void test_version_compare() {
  assert(compare_versions("0.0.3", "0.0.4") < 0);
  assert(compare_versions("0.0.4", "0.0.3") > 0);
  assert(compare_versions("0.0.3", "0.0.3") == 0);
  assert(compare_versions("v0.0.4", "0.0.3") > 0);  // leading 'v' ignored
  assert(compare_versions("0.0.10", "0.0.9") > 0);  // numeric, not lexicographic
  assert(compare_versions("0.1.0", "0.0.99") > 0);
  assert(compare_versions("0.1", "0.1.0") == 0);  // missing fields count as 0
  assert(compare_versions("1.0.0", "0.9.9") > 0);

  assert(version_newer("0.0.5", "0.0.4"));
  assert(!version_newer("0.0.4", "0.0.4"));
  assert(!version_newer("0.0.3", "0.0.4"));
  // Malformed / pre-release never reads as newer than a real release.
  assert(!version_newer("0.0.5-rc1", "0.0.4"));
  assert(!version_newer("garbage", "0.0.1"));
  assert(!version_newer("", "0.0.1"));
}

static void test_parse_and_summarize() {
  // A minimal GitHub /releases payload: mixed field order, nested objects, and escapes.
  const std::string json =
      "[{\"tag_name\":\"v0.0.5\",\"name\":\"0.0.5 \xE2\x80\x94 the notify release\","
      "\"author\":{\"login\":\"properrr\"},\"assets\":[],"
      "\"body\":\"line one\\r\\nline two\"},"
      "{\"name\":\"0.0.4\",\"tag_name\":\"v0.0.4\",\"body\":\"older notes\"},"
      "{\"tag_name\":\"v0.0.3\",\"body\":\"ancient\"},"
      "{\"name\":\"draft with no tag\",\"body\":\"skipped\"}]";

  const std::vector<ReleaseNote> notes = parse_github_releases(json);
  assert(notes.size() == 3);  // the tag-less entry is dropped
  assert(notes[0].version == "0.0.5");
  assert(notes[0].title == "0.0.5 \xE2\x80\x94 the notify release");
  assert(notes[0].body == "line one\nline two");  // \r dropped, \n kept
  assert(notes[1].version == "0.0.4");

  // Only releases strictly newer than local, newest-first, available_version = the newest.
  const ChangelogView v = summarize_releases(notes, "0.0.3", 4000);
  assert(v.available_version == "0.0.5");
  assert(v.notes.find("0.0.5") != std::string::npos);
  assert(v.notes.find("older notes") != std::string::npos);  // 0.0.4 included (newer than 0.0.3)
  assert(v.notes.find("ancient") == std::string::npos);      // 0.0.3 == local, excluded
  assert(v.notes.find("line one") < v.notes.find("older notes"));

  // Up to date: nothing newer -> empty view.
  const ChangelogView none = summarize_releases(notes, "0.0.5", 4000);
  assert(none.available_version.empty());
  assert(none.notes.empty());

  // Clamp: a tiny budget truncates and marks it.
  const ChangelogView clamped = summarize_releases(notes, "0.0.3", 8);
  assert(clamped.notes.size() <= 8 + 4);  // 8 + the "\n…" marker (… is 3 UTF-8 bytes)

  // Malformed JSON must not crash and yields nothing usable.
  assert(parse_github_releases("not json at all").empty());
  assert(parse_github_releases("").empty());
}

// Release notes are untrusted input: key names inside values, quoted braces, escapes, and nested
// objects must not confuse the parser.
static void test_release_parsing_handles_hostile_strings() {
  const std::string json = R"([
    {
      "tag_name": "v1.2.3",
      "name": "quote \" backslash \\ and a brace }",
      "body": "mentions \"tag_name\": \"v9.9.9\" inside the text\nand • a bullet"
    },
    {"tag_name": "v1.0.0", "name": "plain", "body": "", "assets": [{"name": "nested asset"}]}
  ])";
  const std::vector<ReleaseNote> notes = parse_github_releases(json);
  assert(notes.size() == 2);

  // A quoted brace inside a string must not end the object early, and escapes decode once.
  assert(notes[0].version == "1.2.3");
  assert(notes[0].title == "quote \" backslash \\ and a brace }");
  // The embedded `"tag_name": "v9.9.9"` is body text, not a field.
  assert(notes[0].body.find("v9.9.9") != std::string::npos);
  assert(notes[0].body.find("\xE2\x80\xA2") != std::string::npos);  // • -> UTF-8 bullet

  // A nested object's "name" must not be mistaken for the release's own.
  assert(notes[1].version == "1.0.0");
  assert(notes[1].title == "plain");

  // A top-level object (GitHub's error payload shape) is not an array of releases.
  assert(parse_github_releases(R"({"message":"API rate limit exceeded"})").empty());
}

static void test_notes_to_plain() {
  // A CHANGELOG section: ATX headings, "-" bullets, **bold**, `code`, and blank-line runs.
  const std::string md =
      "### Added\r\n"
      "- **Self-update** notify mode: see `self_update_mode`.\r\n"
      "- Another item\r\n"
      "\r\n"
      "\r\n"
      "### Fixed\r\n"
      "- A `bug`\r\n";
  const std::string txt = notes_to_plain(md);
  assert(txt.find("###") == std::string::npos);                       // heading hashes stripped
  assert(txt.find("**") == std::string::npos);                        // bold markers stripped
  assert(txt.find('`') == std::string::npos);                         // code fences stripped
  assert(txt.find("Added") != std::string::npos);                     // heading text kept
  assert(txt.find("\xE2\x80\xA2 Self-update") != std::string::npos);  // "- " -> "• "
  assert(txt.find("self_update_mode") != std::string::npos);
  assert(txt.find("\n\n\n") == std::string::npos);  // blank-line runs collapsed
  assert(txt.front() != '\n' && txt.back() != '\n');

  assert(notes_to_plain("").empty());
  assert(notes_to_plain("plain text, no markdown") == "plain text, no markdown");
}

static void test_decision() {
  // A brand-new commit: show both.
  NotifyDecision d = decide_notification("cccc", "", "");
  assert(d.show_card && d.show_dot);

  // Card already shown for this commit, dot not dismissed: dot only.
  d = decide_notification("cccc", "cccc", "");
  assert(!d.show_card && d.show_dot);

  // Dot dismissed but card unseen: card only.
  d = decide_notification("cccc", "", "cccc");
  assert(d.show_card && !d.show_dot);

  // Both recorded for this commit: nothing auto-surfaces.
  d = decide_notification("cccc", "cccc", "cccc");
  assert(!d.show_card && !d.show_dot);

  // A NEWER commit re-arms both even though older ones were shown/dismissed.
  d = decide_notification("dddd", "cccc", "cccc");
  assert(d.show_card && d.show_dot);

  // No commit at all: surface nothing.
  d = decide_notification("", "", "");
  assert(!d.show_card && !d.show_dot);
}

static void test_markers() {
  namespace fs = std::filesystem;
  const fs::path dir = deckback_test_tmpdir() / "deckback_updateprompt_test";
  fs::remove_all(dir);
  const std::string sdir = dir.string();

  const std::string card_path = update_card_marker_path(sdir);
  const std::string dot_path = update_dot_marker_path(sdir);
  assert(card_path != dot_path);

  // Absent -> "".
  assert(read_update_marker(card_path).empty());

  // Round-trip (write creates the dir).
  assert(write_update_marker(card_path, "commitABCD"));
  assert(read_update_marker(card_path) == "commitABCD");
  assert(write_update_marker(dot_path, "commitEFGH"));
  assert(read_update_marker(dot_path) == "commitEFGH");

  // A path under a regular file cannot be created (even by root): returns false, never throws.
  const std::string blocker = sdir + "/blocker";
  assert(write_update_marker(blocker, "x"));   // a normal file
  const std::string bad = blocker + "/child";  // "blocker" is a file, so this parent is invalid
  assert(!write_update_marker(bad, "y"));
  assert(read_update_marker(bad).empty());

  fs::remove_all(dir);
}

// The notes for one specific version (the one running), for the Updates tab.
void test_release_notes_for() {
  const std::vector<ReleaseNote> notes = {
      {"0.0.8", "Deckback v0.0.8", "### Fixed\n- Self-update works again"},
      {"0.0.7", "Deckback v0.0.7", "### Added\n- Captions"},
      {"0.0.6", "", "bare body"},
  };
  const std::string cur = release_notes_for(notes, "0.0.7", 4000);
  assert(cur.find("Deckback v0.0.7") == 0);
  assert(cur.find("Captions") != std::string::npos);
  assert(cur.find("Self-update") == std::string::npos);  // only the asked-for version

  // A release with no name falls back to the tag, so the heading is never empty.
  assert(release_notes_for(notes, "0.0.6", 4000).find("v0.0.6") == 0);
  // An unknown version yields nothing rather than the closest match.
  assert(release_notes_for(notes, "0.9.9", 4000).empty());
  // Equivalent-but-differently-written versions still match ("0.0.7" == "0.0.7.0").
  assert(!release_notes_for(notes, "0.0.7.0", 4000).empty());
  // The clamp applies.
  assert(release_notes_for(notes, "0.0.7", 8).size() <= 8 + 4);
}

// The status text and buttons the Updates tab shows for each deploy phase.
void test_updates_view() {
  const std::string_view idle = "No update announced yet.";

  // Idle, updater live: only a manual check, and the idle text is passed through verbatim.
  UpdatesView v = updates_view(DeployPhase::Idle, "", false, "", "0.0.6", idle, true);
  assert(v.status == idle && !v.has_update && v.can_check);

  // Self-update off: nothing offered at all.
  v = updates_view(DeployPhase::Idle, "", false, "", "0.0.6", idle, false);
  assert(!v.has_update && !v.can_check);

  // Announced update: the deploy pair, and the version named.
  v = updates_view(DeployPhase::Idle, "", true, "0.0.7", "0.0.6", idle, true);
  assert(v.has_update && !v.can_check);
  assert(v.status.find("0.0.7") != std::string::npos);

  // In flight: no buttons and no claim of success.
  v = updates_view(DeployPhase::Requested, "", true, "0.0.7", "0.0.6", idle, true);
  assert(!v.has_update && !v.can_check);
  assert(v.status.find("Updating") != std::string::npos);

  // Only Done promises a restart will apply it.
  v = updates_view(DeployPhase::Done, "", false, "", "0.0.6", idle, true);
  assert(v.status.find("Restart") != std::string::npos);
  assert(!v.has_update && !v.can_check);

  // Empty means the portal compared against the remote, so it may say "latest".
  v = updates_view(DeployPhase::Empty, "", false, "", "0.0.6", idle, true);
  assert(v.status.find("latest") != std::string::npos && v.can_check);

  // A generic failure surfaces the portal's own message and allows a retry.
  v = updates_view(DeployPhase::Failed, "network unreachable", true, "0.0.7", "0.0.6", idle, true);
  assert(v.status.find("network unreachable") != std::string::npos);
  assert(v.can_check);

  // A permission refusal offers no buttons; the text says how to update from the host.
  v = updates_view(DeployPhase::PermissionBlocked, "Self update not supported", true, "0.0.7",
                   "0.0.6", idle, true);
  assert(!v.has_update && !v.can_check);
  assert(v.status.find("Desktop") != std::string::npos);
  assert(v.status.find("install.sh") != std::string::npos);

  // A deploy verdict outranks a still-set availability flag.
  v = updates_view(DeployPhase::Done, "", true, "0.0.7", "0.0.6", idle, true);
  assert(!v.has_update);
}

// Which deploy verdicts raise a toast: terminal ones only.
void test_deploy_toast() {
  assert(deploy_toast(DeployPhase::Done).find("restart") != std::string::npos);
  assert(deploy_toast(DeployPhase::PermissionBlocked).find("Desktop Mode") != std::string::npos);
  assert(!deploy_toast(DeployPhase::Failed).empty());
  // Empty answers a user's explicit check, so it speaks too.
  assert(deploy_toast(DeployPhase::Empty).find("latest") != std::string::npos);
  // Non-terminal states have no verdict to announce.
  assert(deploy_toast(DeployPhase::Requested).empty());
  assert(deploy_toast(DeployPhase::Idle).empty());
}

// After a check answered Empty, a commit announced later in the session is offered again.
void test_new_commit_after_an_empty_check_is_announced() {
  namespace fs = std::filesystem;
  const fs::path dir = deckback_test_tmpdir() / "deckback_updateprompt_recheck";
  fs::remove_all(dir);
  fs::create_directories(dir);

  OsdMenuConfig ocfg;
  ocfg.cdp_port = 1;  // nothing listens: the menu stays closed, only its update model is observed
  OsdMenuController osd(ocfg);
  UpdateState state;
  UpdatePromptConfig cfg;
  cfg.cdp_port = 1;
  cfg.state_dir = dir.string();
  cfg.local_version = "0.0.9";
  cfg.releases_url = "http://127.0.0.1:1/releases";  // refused at once: no network in a unit test
  cfg.state = &state;
  cfg.osd = &osd;
  const std::unique_ptr<Updater> updater = Updater::create(UpdaterConfig{});  // never started
  cfg.updater = updater.get();
  {
    UpdatePromptController ctl(cfg);
    ctl.check_now();
    ctl.on_deploy_result(UpdateProgress::Empty, "", "");
    ctl.tick(false);
    assert(!osd.update_available());

    state.set_available(true, "abc123def456");
    ctl.tick(false);
    assert(osd.update_available());
  }
  fs::remove_all(dir);
}

// A deploy that never gets a verdict times out to Failed.
void test_a_deploy_that_never_answers_times_out() {
  namespace fs = std::filesystem;
  const fs::path dir = deckback_test_tmpdir() / "deckback_updateprompt_timeout";
  fs::remove_all(dir);
  fs::create_directories(dir);

  OsdMenuConfig ocfg;
  ocfg.cdp_port = 1;
  OsdMenuController osd(ocfg);
  UpdateState state;
  UpdatePromptConfig cfg;
  cfg.cdp_port = 1;
  cfg.state_dir = dir.string();
  cfg.local_version = "0.0.9";
  cfg.releases_url = "http://127.0.0.1:1/releases";
  cfg.state = &state;
  cfg.osd = &osd;
  cfg.deploy_timeout_ms = 50;
  const std::unique_ptr<Updater> updater = Updater::create(UpdaterConfig{});  // never answers
  cfg.updater = updater.get();
  {
    UpdatePromptController ctl(cfg);
    ctl.check_now();
    ctl.tick(false);
    assert(ctl.deploy_phase() == DeployPhase::Requested);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ctl.tick(false);
    assert(ctl.deploy_phase() == DeployPhase::Failed);
    // A later verdict still lands.
    ctl.check_now();
    ctl.on_deploy_result(UpdateProgress::Done, "", "");
    ctl.tick(false);
    assert(ctl.deploy_phase() == DeployPhase::Done);
  }
  fs::remove_all(dir);
}

// ---- published-ref probe ------------------------------------------------------------------------

const std::string kRunning(64, 'a');
const std::string kNewer(64, 'b');

void test_ref_check() {
  // The ref file is 64 lowercase hex and a newline; anything else is not a commit.
  assert(parse_ref_commit(kNewer + "\n") == kNewer);
  assert(parse_ref_commit("  " + kNewer + "\r\n") == kNewer);
  assert(parse_ref_commit(std::string(64, 'B')).empty());  // ostree never writes uppercase
  assert(parse_ref_commit(std::string(63, 'b')).empty());
  assert(parse_ref_commit(kNewer + "0").empty());
  assert(parse_ref_commit("<html><body>404</body></html>").empty());
  assert(parse_ref_commit("").empty());

  assert(decide_ref_check(kRunning + "\n", kRunning) == RefCheck::UpToDate);
  // Any different commit is an update (a recall is a new commit too).
  assert(decide_ref_check(kNewer + "\n", kRunning) == RefCheck::UpdateAvailable);
  // An unreadable body is Unknown, never "latest".
  assert(decide_ref_check("Not Found", kRunning) == RefCheck::Unknown);
  assert(decide_ref_check(kNewer, "") == RefCheck::Unknown);
  assert(decide_ref_check(kNewer, "not-a-commit") == RefCheck::Unknown);
}

void test_deploy_progress_status() {
  // flatpak-portal's op is 0-based; a user counts from 1.
  assert(deploy_progress_status(1, 3, 45) == "Updating\xE2\x80\xA6 step 2 of 3 (45%)");
  assert(deploy_progress_status(0, 1, 7) == "Updating\xE2\x80\xA6 7%");
  assert(deploy_progress_status(0, 0, 7) == "Updating\xE2\x80\xA6 7%");
  // Out-of-range input is clamped.
  assert(deploy_progress_status(5, 3, 250) == "Updating\xE2\x80\xA6 step 3 of 3 (100%)");
}

void test_updates_view_for_the_new_phases() {
  const std::string_view idle = "No update announced yet.";
  // Checking: nothing to press until it answers.
  UpdatesView v = updates_view(DeployPhase::Checking, "", false, "", "0.0.9", idle, true);
  assert(v.status.find("Checking") != std::string::npos);
  assert(!v.has_update && !v.can_check && !v.can_quit);
  // Progress replaces the generic in-flight text, still with no buttons.
  v = updates_view(DeployPhase::Requested, "", false, "", "0.0.9", idle, true,
                   deploy_progress_status(0, 2, 30));
  assert(v.status.find("step 1 of 2") != std::string::npos && !v.can_check);
  // A staged update offers only Quit.
  v = updates_view(DeployPhase::Done, "", false, "", "0.0.9", idle, true);
  assert(v.can_quit && !v.can_check && !v.has_update);
}

// Counts portal deploy requests instead of making them.
class FakeUpdater final : public Updater {
 public:
  void start() override {}
  void stop() override {}
  void request_update() override { ++deploys; }
  bool backend_live() const override { return true; }
  int selftest() override { return 0; }
  int selftest_deploy(bool) override { return 0; }
  std::atomic<int> deploys{0};
};

OsdMenuConfig closed_osd() {
  OsdMenuConfig c;
  c.cdp_port = 1;  // nothing listens: the menu stays closed, only its update model is observed
  return c;
}

// A closed OSD, shared state, a counting updater, and a repo ref body the test sets (nullopt =
// unreachable).
struct ProbeRig {
  explicit ProbeRig(const char* name) : dir(deckback_test_tmpdir() / name), osd(closed_osd()) {
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  }
  ~ProbeRig() { std::filesystem::remove_all(dir); }

  void serve(std::optional<std::string> body) {
    std::lock_guard lk(mu);
    ref = std::move(body);
  }

  UpdatePromptConfig config() {
    UpdatePromptConfig cfg;
    cfg.cdp_port = 1;
    cfg.state_dir = dir.string();
    cfg.local_version = "0.0.9";
    cfg.releases_url = "http://127.0.0.1:1/releases";  // refused at once: no network here
    cfg.state = &state;
    cfg.updater = &updater;
    cfg.osd = &osd;
    cfg.running_commit = kRunning;
    cfg.fetch_remote_ref = [this]() -> std::optional<std::string> {
      ++fetches;
      std::lock_guard lk(mu);
      return ref;
    };
    return cfg;
  }

  std::filesystem::path dir;
  OsdMenuController osd;
  UpdateState state;
  FakeUpdater updater;
  std::atomic<int> fetches{0};
  std::mutex mu;
  std::optional<std::string> ref;
};

template <class Done>
bool tick_until(UpdatePromptController& ctl, Done done) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < end) {
    ctl.tick(false);
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

// The launch probe announces a newer commit without the portal and without installing anything.
void test_launch_probe_announces_a_newer_commit() {
  ProbeRig rig("deckback_probe_launch");
  rig.serve(kNewer + "\n");
  UpdatePromptController ctl(rig.config());
  assert(tick_until(ctl, [&] { return rig.osd.update_available(); }));
  // Keyed like the portal's announcement, so the markers see one release.
  assert(rig.state.commit() == short_commit(kNewer));
  assert(rig.updater.deploys == 0);
  assert(rig.fetches == 1);
}

// Up to date at launch: the phase becomes Empty ("latest") without a deploy.
void test_launch_probe_up_to_date_says_latest() {
  ProbeRig rig("deckback_probe_latest");
  rig.serve(kRunning + "\n");
  UpdatePromptController ctl(rig.config());
  assert(tick_until(ctl, [&] { return ctl.deploy_phase() == DeployPhase::Empty; }));
  assert(!rig.osd.update_available() && rig.updater.deploys == 0);
}

// A probe reading "up to date" (the CDN caches the ref) never clears a portal announcement.
void test_probe_never_clears_a_portal_announcement() {
  ProbeRig rig("deckback_probe_noclear");
  rig.serve(kRunning + "\n");
  rig.state.set_available(true, short_commit(kNewer));
  UpdatePromptController ctl(rig.config());
  assert(tick_until(ctl, [&] { return rig.fetches == 1; }));
  for (int i = 0; i < 20; ++i) {
    ctl.tick(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  assert(ctl.deploy_phase() == DeployPhase::Idle);
  assert(rig.osd.update_available());
}

// "Check for updates" reads the ref and installs nothing; only confirm_update deploys.
void test_check_reads_the_ref_and_installs_nothing() {
  ProbeRig rig("deckback_probe_check");
  rig.serve(kRunning + "\n");
  UpdatePromptController ctl(rig.config());
  assert(tick_until(ctl, [&] { return ctl.deploy_phase() == DeployPhase::Empty; }));

  rig.serve(kNewer + "\n");  // a release lands mid-session
  ctl.check_now();
  assert(ctl.deploy_phase() == DeployPhase::Checking);
  assert(tick_until(ctl, [&] { return rig.osd.update_available(); }));
  assert(ctl.deploy_phase() == DeployPhase::Idle);  // the announcement is the answer
  assert(rig.updater.deploys == 0);

  ctl.confirm_update();  // consent is what deploys
  assert(rig.updater.deploys == 1);
}

// An unreadable repo falls back to the portal's check-by-deploying.
void test_check_with_an_unreachable_repo_falls_back_to_the_portal() {
  ProbeRig rig("deckback_probe_fallback");
  rig.serve(std::nullopt);
  UpdatePromptController ctl(rig.config());
  assert(tick_until(ctl, [&] { return rig.fetches == 1; }));
  ctl.tick(false);
  assert(ctl.deploy_phase() == DeployPhase::Idle);  // a failed LAUNCH probe changes nothing

  ctl.check_now();
  assert(tick_until(ctl, [&] { return ctl.deploy_phase() == DeployPhase::Requested; }));
  assert(rig.updater.deploys == 1);
}

// Without a running commit (outside the sandbox) the check goes straight to the portal.
void test_no_running_commit_keeps_the_portal_check() {
  ProbeRig rig("deckback_probe_unsandboxed");
  rig.serve(kNewer + "\n");
  UpdatePromptConfig cfg = rig.config();
  cfg.running_commit.clear();
  UpdatePromptController ctl(cfg);
  ctl.tick(false);
  ctl.check_now();
  assert(ctl.deploy_phase() == DeployPhase::Requested);
  assert(rig.updater.deploys == 1 && rig.fetches == 0);
}

// A staged update already answers "is there something newer": no probe may re-offer it.
void test_a_staged_update_is_not_re_announced() {
  ProbeRig rig("deckback_probe_staged");
  rig.serve(kNewer + "\n");
  UpdatePromptController ctl(rig.config());
  assert(tick_until(ctl, [&] { return rig.osd.update_available(); }));
  ctl.confirm_update();
  ctl.on_deploy_result(UpdateProgress::Done, "", "");
  ctl.tick(false);
  assert(ctl.deploy_phase() == DeployPhase::Done);
  ctl.check_now();  // the tab offers no Check here; a stray call must not change anything
  ctl.tick(false);
  assert(ctl.deploy_phase() == DeployPhase::Done && rig.fetches == 1);
  assert(!rig.osd.update_available());
}

// A commit the portal found nothing to install for is not offered again by later checks.
void test_a_commit_the_portal_refused_is_not_re_announced() {
  ProbeRig rig("deckback_probe_refused");
  rig.serve(kNewer + "\n");
  UpdatePromptController ctl(rig.config());
  assert(tick_until(ctl, [&] { return rig.osd.update_available(); }));
  ctl.confirm_update();
  ctl.on_deploy_result(UpdateProgress::Empty, "", "");
  assert(tick_until(ctl, [&] { return ctl.deploy_phase() == DeployPhase::Empty; }));

  ctl.check_now();
  assert(tick_until(ctl, [&] { return ctl.deploy_phase() == DeployPhase::Empty; }));
  assert(!rig.osd.update_available());
  assert(rig.updater.deploys == 1 && rig.fetches == 2);
}

// After a failed deploy, "Check for updates" offers the same commit again.
void test_check_after_a_failed_deploy_offers_the_update_again() {
  ProbeRig rig("deckback_probe_retry");
  rig.serve(kNewer + "\n");
  UpdatePromptController ctl(rig.config());
  assert(tick_until(ctl, [&] { return rig.osd.update_available(); }));
  ctl.confirm_update();
  ctl.on_deploy_result(UpdateProgress::Failed, "", "network unreachable");
  assert(tick_until(ctl, [&] { return ctl.deploy_phase() == DeployPhase::Failed; }));
  assert(!rig.osd.update_available());

  ctl.check_now();
  assert(tick_until(ctl, [&] { return rig.osd.update_available(); }));
  assert(ctl.deploy_phase() == DeployPhase::Idle);
  ctl.confirm_update();
  assert(rig.updater.deploys == 2);
}

// The refusal mark expires with the CDN window, after which a check offers the commit again.
void test_a_refusal_expires_with_the_cdn_window() {
  ProbeRig rig("deckback_probe_refusal_window");
  rig.serve(kNewer + "\n");
  UpdatePromptConfig cfg = rig.config();
  cfg.refusal_window_ms = 50;
  UpdatePromptController ctl(cfg);
  assert(tick_until(ctl, [&] { return rig.osd.update_available(); }));
  ctl.confirm_update();
  ctl.on_deploy_result(UpdateProgress::Empty, "", "");
  assert(tick_until(ctl, [&] { return ctl.deploy_phase() == DeployPhase::Empty; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  ctl.check_now();
  assert(tick_until(ctl, [&] { return rig.osd.update_available(); }));
}

// Each progress report restarts the no-answer timeout; silence still times out.
void test_progress_keeps_a_slow_deploy_alive() {
  ProbeRig rig("deckback_probe_progress");
  rig.serve(kNewer + "\n");
  UpdatePromptConfig cfg = rig.config();
  cfg.deploy_timeout_ms = 150;
  UpdatePromptController ctl(cfg);
  assert(tick_until(ctl, [&] { return rig.osd.update_available(); }));
  ctl.confirm_update();
  for (int i = 0; i < 6; ++i) {  // 6 x 50 ms = twice the timeout, with progress throughout
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ctl.on_deploy_progress(0, 1, static_cast<std::uint32_t>(i * 15));
    ctl.tick(false);
    assert(ctl.deploy_phase() == DeployPhase::Requested);
  }
  assert(tick_until(ctl, [&] { return ctl.deploy_phase() == DeployPhase::Failed; }));
}

DECKBACK_TEST_MAIN(updateprompt) {
  test_version_compare();
  test_parse_and_summarize();
  test_release_parsing_handles_hostile_strings();
  test_notes_to_plain();
  test_decision();
  test_markers();
  test_release_notes_for();
  test_updates_view();
  test_deploy_toast();
  test_new_commit_after_an_empty_check_is_announced();
  test_a_deploy_that_never_answers_times_out();
  test_ref_check();
  test_deploy_progress_status();
  test_updates_view_for_the_new_phases();
  test_launch_probe_announces_a_newer_commit();
  test_launch_probe_up_to_date_says_latest();
  test_probe_never_clears_a_portal_announcement();
  test_check_reads_the_ref_and_installs_nothing();
  test_check_with_an_unreachable_repo_falls_back_to_the_portal();
  test_no_running_commit_keeps_the_portal_check();
  test_a_staged_update_is_not_re_announced();
  test_a_commit_the_portal_refused_is_not_re_announced();
  test_progress_keeps_a_slow_deploy_alive();
  test_check_after_a_failed_deploy_offers_the_update_again();
  test_a_refusal_expires_with_the_cdn_window();
  std::puts("updateprompt: ok");
  return 0;
}
