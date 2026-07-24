// L0 for the self-update NOTIFY UI's pure logic (findings durable/self-update.md). The portal
// deploy and the on-page card/dot rendering are verified on a Deck; here we pin the parts that must
// be correct off-Deck: semantic version comparison (a wrong answer offers a downgrade or hides a
// real update), GitHub-releases parsing + changelog summary (what the card shows), the
// show-card/show-dot decision (the "painless, ignorable, reachable" contract), and the dismissal
// state files (round-trip + read-only tolerance).
#include "updateprompt.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>

#include "harness.hpp"

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
  // "Nothing to do" must not interrupt: the tab the user is looking at already says it.
  assert(deploy_toast(DeployPhase::Empty).empty());
  assert(deploy_toast(DeployPhase::Requested).empty());
  assert(deploy_toast(DeployPhase::Idle).empty());
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
  std::puts("updateprompt: ok");
  return 0;
}
