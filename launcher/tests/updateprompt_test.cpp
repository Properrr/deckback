// L0 for the self-update NOTIFY UI's pure logic (findings durable/self-update.md). The portal
// deploy and the on-page card/dot rendering are verified on a Deck; here we pin the parts that must
// be correct off-Deck: semantic version comparison (a wrong answer offers a downgrade or hides a
// real update), GitHub-releases parsing + changelog summary (what the card shows), the
// show-card/show-dot decision (the "painless, ignorable, reachable" contract), and the dismissal
// state files (round-trip + read-only tolerance).
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
  // A minimal GitHub /releases payload: fields in mixed order, nested objects (author/assets), and
  // escapes in the body. Newest first, but summarize must not rely on input order.
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
  // 0.0.5's notes come before 0.0.4's.
  assert(v.notes.find("line one") < v.notes.find("older notes"));

  // Up to date: nothing newer -> empty view (the card then shows the fallback link).
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

// These are the cases the hand-rolled byte-scanner this parser replaced got wrong. It found a key
// with an unanchored find() over the raw text and read whatever followed the next colon, so a value
// that merely *contained* the key name won; and it decoded escapes itself. Release notes are
// attacker-adjacent input we render into an overlay, so the escaping has to be the real thing.
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
  // The embedded `"tag_name": "v9.9.9"` is text, not a field: the version above must be 1.2.3.
  assert(notes[0].body.find("v9.9.9") != std::string::npos);
  assert(notes[0].body.find("\xE2\x80\xA2") != std::string::npos);  // • -> UTF-8 bullet

  // A nested object's "name" must not be mistaken for the release's own.
  assert(notes[1].version == "1.0.0");
  assert(notes[1].title == "plain");

  // A top-level object (GitHub's error payload shape) is not an array of releases.
  assert(parse_github_releases(R"({"message":"API rate limit exceeded"})").empty());
}

static void test_notes_to_plain() {
  // A CHANGELOG "## [x]" section as GitHub delivers it: ATX subsection headings, "-" bullets,
  // **bold**, `code`, and blank-line runs. The card must show clean 10-foot text.
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
  // A brand-new commit: show both. (The painless first-notification case.)
  NotifyDecision d = decide_notification("cccc", "", "");
  assert(d.show_card && d.show_dot);

  // Card already auto-shown for this commit, dot not dismissed: no re-nag, dot persists.
  d = decide_notification("cccc", "cccc", "");
  assert(!d.show_card && d.show_dot);

  // Dot dismissed ("ignore this version") but card unseen: dot hidden — MUST return a suppress, or
  // "ignore forever" would be a check that cannot fail (HARNESS.md).
  d = decide_notification("cccc", "", "cccc");
  assert(d.show_card && !d.show_dot);

  // Both recorded for this commit: nothing auto-surfaces (the Menu route stays reachable
  // elsewhere).
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
  const fs::path dir = fs::temp_directory_path() / "deckback_updateprompt_test";
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

  // A path that can't be created (its "parent" is a regular file) warns and returns false — never
  // throws. Deterministic and root-safe: even root cannot mkdir under a regular file.
  const std::string blocker = sdir + "/blocker";
  assert(write_update_marker(blocker, "x"));   // a normal file
  const std::string bad = blocker + "/child";  // "blocker" is a file, so this parent is invalid
  assert(!write_update_marker(bad, "y"));
  assert(read_update_marker(bad).empty());

  fs::remove_all(dir);
}

// "What's new in the version you are running". summarize_releases only keeps releases NEWER than
// local, so before this the Updates tab was blank whenever nothing was pending — which is nearly
// always, and is what a user meant by "I didn't see a changelog" after restarting.
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
  // The clamp applies, so a huge body cannot blow up the OSD payload.
  assert(release_notes_for(notes, "0.0.7", 8).size() <= 8 + 4);
}

// The status/buttons the Updates tab shows. This is the function the v0.0.7 report was really
// about: the tab claimed the update would apply after a restart while the portal had already
// refused it, because the claim was made at button-press time and nothing ever revised it.
void test_updates_view() {
  const std::string_view idle = "No update announced yet.";

  // Idle, updater live: only a manual check, and the idle text is passed through verbatim — the
  // view must never invent a "you are up to date" the app has not earned.
  UpdatesView v = updates_view(DeployPhase::Idle, "", false, "", "0.0.6", idle, true);
  assert(v.status == idle && !v.has_update && v.can_check);

  // Self-update off: nothing offered at all.
  v = updates_view(DeployPhase::Idle, "", false, "", "0.0.6", idle, false);
  assert(!v.has_update && !v.can_check);

  // Announced update: the deploy pair, and the version named.
  v = updates_view(DeployPhase::Idle, "", true, "0.0.7", "0.0.6", idle, true);
  assert(v.has_update && !v.can_check);
  assert(v.status.find("0.0.7") != std::string::npos);

  // In flight: no buttons, and above all no claim of success.
  v = updates_view(DeployPhase::Requested, "", true, "0.0.7", "0.0.6", idle, true);
  assert(!v.has_update && !v.can_check);
  assert(v.status.find("Updating") != std::string::npos);

  // Done is the ONLY state that may promise a restart will apply it.
  v = updates_view(DeployPhase::Done, "", false, "", "0.0.6", idle, true);
  assert(v.status.find("Restart") != std::string::npos);
  assert(!v.has_update && !v.can_check);

  // Empty means the portal really did compare against the remote, so "latest" is earned here.
  v = updates_view(DeployPhase::Empty, "", false, "", "0.0.6", idle, true);
  assert(v.status.find("latest") != std::string::npos && v.can_check);

  // A generic failure surfaces the portal's own message and allows a retry.
  v = updates_view(DeployPhase::Failed, "network unreachable", true, "0.0.7", "0.0.6", idle, true);
  assert(v.status.find("network unreachable") != std::string::npos);
  assert(v.can_check);

  // The permission refusal offers NOTHING: every button would fail identically. The text has to
  // carry the only thing that works — updating from the host.
  v = updates_view(DeployPhase::PermissionBlocked, "Self update not supported", true, "0.0.7",
                   "0.0.6", idle, true);
  assert(!v.has_update && !v.can_check);
  assert(v.status.find("Desktop") != std::string::npos);
  assert(v.status.find("install.sh") != std::string::npos);

  // A deploy verdict outranks a still-set availability flag: it is the more recent truth.
  v = updates_view(DeployPhase::Done, "", true, "0.0.7", "0.0.6", idle, true);
  assert(!v.has_update);
}

// Which verdicts interrupt the user. A toast is a modal-ish interruption over playback, so it is
// reserved for outcomes that change what they should do.
void test_deploy_toast() {
  assert(deploy_toast(DeployPhase::Done).find("restart") != std::string::npos);
  assert(deploy_toast(DeployPhase::PermissionBlocked).find("Desktop Mode") != std::string::npos);
  assert(!deploy_toast(DeployPhase::Failed).empty());
  // Empty MUST speak. The user pressed a button to ask; a silent answer is indistinguishable from a
  // dead button, which is exactly how it was reported on-device. (The caller suppresses this while
  // the Updates panel is open, since the panel says the same thing.)
  assert(deploy_toast(DeployPhase::Empty).find("latest") != std::string::npos);
  // Non-terminal states have no verdict to announce.
  assert(deploy_toast(DeployPhase::Requested).empty());
  assert(deploy_toast(DeployPhase::Idle).empty());
}

// "Check for updates" answered Empty, then the portal announces a commit published later in the
// session: the badge and the Update button must come back. A verdict from an earlier deploy used to
// pin the phase for the rest of the session, so the panel kept saying "latest" over a real update.
void test_new_commit_after_an_empty_check_is_announced() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "deckback_updateprompt_recheck";
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

// An Update() that never gets a verdict -- a hung portal transaction, or an updater that is not
// running -- must not leave the tab on "Updating…" with no buttons until the next launch.
void test_a_deploy_that_never_answers_times_out() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "deckback_updateprompt_timeout";
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
    // ...and a verdict that does arrive still lands: the timeout is not a latch.
    ctl.check_now();
    ctl.on_deploy_result(UpdateProgress::Done, "", "");
    ctl.tick(false);
    assert(ctl.deploy_phase() == DeployPhase::Done);
  }
  fs::remove_all(dir);
}

// ---- the published-ref probe (U4)
// ----------------------------------------------------------------

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
  // "Differs" is the update: the ref is what flatpak would move to, and a recall is a new commit.
  assert(decide_ref_check(kNewer + "\n", kRunning) == RefCheck::UpdateAvailable);
  // An unreadable body must not read as "latest" — that is exactly the claim it cannot support.
  assert(decide_ref_check("Not Found", kRunning) == RefCheck::Unknown);
  assert(decide_ref_check(kNewer, "") == RefCheck::Unknown);
  assert(decide_ref_check(kNewer, "not-a-commit") == RefCheck::Unknown);
}

void test_deploy_progress_status() {
  // flatpak-portal's op is 0-based; a user counts from 1.
  assert(deploy_progress_status(1, 3, 45) == "Updating\xE2\x80\xA6 step 2 of 3 (45%)");
  assert(deploy_progress_status(0, 1, 7) == "Updating\xE2\x80\xA6 7%");
  assert(deploy_progress_status(0, 0, 7) == "Updating\xE2\x80\xA6 7%");
  // Out-of-range input is clamped, never "step 4 of 3" or "250%".
  assert(deploy_progress_status(5, 3, 250) == "Updating\xE2\x80\xA6 step 3 of 3 (100%)");
}

void test_updates_view_for_the_new_phases() {
  const std::string_view idle = "No update announced yet.";
  // Checking: a read of the ref, nothing to press until it answers.
  UpdatesView v = updates_view(DeployPhase::Checking, "", false, "", "0.0.9", idle, true);
  assert(v.status.find("Checking") != std::string::npos);
  assert(!v.has_update && !v.can_check && !v.can_quit);
  // Progress replaces the generic in-flight text, still with no buttons.
  v = updates_view(DeployPhase::Requested, "", false, "", "0.0.9", idle, true,
                   deploy_progress_status(0, 2, 30));
  assert(v.status.find("step 1 of 2") != std::string::npos && !v.can_check);
  // A staged update offers the one action that moves it forward.
  v = updates_view(DeployPhase::Done, "", false, "", "0.0.9", idle, true);
  assert(v.can_quit && !v.can_check && !v.has_update);
}

// Records portal deploys instead of making them, so "a check installs nothing" is an assertion.
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

// One controller's world: a closed OSD, the shared state, a counting updater and a "repo" whose ref
// body the test sets (nullopt = unreachable).
struct ProbeRig {
  explicit ProbeRig(const char* name)
      : dir(std::filesystem::temp_directory_path() / name), osd(closed_osd()) {
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

// The launch probe announces a release published since the last session, without the portal and
// without installing anything.
void test_launch_probe_announces_a_newer_commit() {
  ProbeRig rig("deckback_probe_launch");
  rig.serve(kNewer + "\n");
  UpdatePromptController ctl(rig.config());
  assert(tick_until(ctl, [&] { return rig.osd.update_available(); }));
  // Keyed exactly like the portal's announcement, or the ignore/card markers would treat the same
  // release as two.
  assert(rig.state.commit() == short_commit(kNewer));
  assert(rig.updater.deploys == 0);
  assert(rig.fetches == 1);
}

// Up to date at launch: the tab may say "latest", quietly, because the repo was really read.
void test_launch_probe_up_to_date_says_latest() {
  ProbeRig rig("deckback_probe_latest");
  rig.serve(kRunning + "\n");
  UpdatePromptController ctl(rig.config());
  assert(tick_until(ctl, [&] { return ctl.deploy_phase() == DeployPhase::Empty; }));
  assert(!rig.osd.update_available() && rig.updater.deploys == 0);
}

// The CDN caches the ref for 10 min, so the portal can know about a release first. A probe that
// still reads "up to date" must not hide it.
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

// "Check for updates" asks and answers; it no longer installs behind notify mode's consent step.
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

// An unreadable repo falls back to the portal's check-by-deploying, the path verified on hardware.
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

// Without a running commit (outside the sandbox) there is nothing to compare: the old behaviour.
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

// The portal found nothing to install for the commit the probe announced (a ref ahead of the
// portal's summary, or an install from another remote): later checks must not keep offering it.
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

// A slow deploy that keeps reporting progress is alive: the no-answer timeout restarts on each
// report instead of calling it failed mid-download.
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
  // ...and silence still times out.
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
  std::puts("updateprompt: ok");
  return 0;
}
