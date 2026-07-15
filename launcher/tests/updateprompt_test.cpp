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

int main() {
  test_version_compare();
  test_parse_and_summarize();
  test_notes_to_plain();
  test_decision();
  test_markers();
  std::puts("updateprompt: ok");
  return 0;
}
