// The user.json overlay (config_store.cpp) and the json::dump writer it relies on. Pure + a
// tmpfile round-trip: allowlist enforcement, sparse set/remove, atomic save, and apply-onto-Config.
#include "config_store.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "config.hpp"
#include "harness.hpp"
#include "json.hpp"

using namespace deckback;

namespace {

namespace fs = std::filesystem;

void write_file(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
}

bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// ---- json::dump ---------------------------------------------------------------------------------

void test_dump_round_trips_through_parse() {
  auto v = json::Value::object({
      {"caption_type", json::Value(std::string("author_first"))},
      {"caption_remember", json::Value(true)},
      {"caption_languages",
       json::Value::array({json::Value(std::string("system")), json::Value(std::string("en"))})},
  });
  const std::string text = json::dump(v);
  assert(has(text, "\"caption_type\": \"author_first\""));
  assert(has(text, "true"));
  json::ParseResult pr = json::parse(text);
  assert(pr.ok());
  assert(*pr.value->find("caption_type")->as_string() == "author_first");
  assert(*pr.value->find("caption_remember")->as_bool() == true);
  assert(pr.value->find("caption_languages")->as_array()->size() == 2);
}

void test_dump_escapes_and_keeps_integers() {
  auto v =
      json::Value::object({{"k", json::Value(std::string("a\"b\\c\nd"))}, {"n", json::Value(5.0)}});
  const std::string text = json::dump(v, -1);
  assert(has(text, "\\\"") && has(text, "\\\\") && has(text, "\\n"));
  assert(has(text, "\"n\":5") && !has(text, "5.0"));
}

// ---- ConfigStore --------------------------------------------------------------------------------

std::string tmp_path(const char* name) { return (fs::temp_directory_path() / name).string(); }

void test_set_persist_reload_apply() {
  const std::string path = tmp_path("deckback_user_test_1.json");
  fs::remove(path);

  {
    ConfigStore s(path);
    s.load();
    assert(s.set_bool("captions_toast", false));
    assert(s.set_string("caption_type", "auto_first"));
    assert(s.set_string_list("caption_languages", {"system", "en", "de"}));
  }
  ConfigStore s2(path);
  s2.load();
  assert(s2.has("caption_type"));
  Config c;
  assert(c.captions_toast == true && c.caption_type == "author_first");
  s2.apply(c);
  assert(c.captions_toast == false);
  assert(c.caption_type == "auto_first");
  assert(c.caption_languages.size() == 3 && c.caption_languages[2] == "de");
  fs::remove(path);
}

void test_non_overlayable_keys_are_refused() {
  const std::string path = tmp_path("deckback_user_test_2.json");
  fs::remove(path);
  ConfigStore s(path);
  s.load();
  assert(!s.set_string("user_agent", "x"));
  assert(!s.set_string("url", "x"));
  assert(!s.has("user_agent"));
  write_file(path, R"({"user_agent":"evil","caption_type":"auto_only"})");
  ConfigStore s2(path);
  s2.load();
  Config c;
  s2.apply(c);
  assert(c.caption_type == "auto_only");
  assert(c.user_agent != "evil");
  fs::remove(path);
}

void test_remove_resets_to_default() {
  const std::string path = tmp_path("deckback_user_test_3.json");
  fs::remove(path);
  ConfigStore s(path);
  s.load();
  s.set_bool("caption_remember", false);
  assert(s.has("caption_remember"));
  s.remove("caption_remember");
  assert(!s.has("caption_remember"));
  Config c;
  s.apply(c);
  assert(c.caption_remember == true);
  assert(s.remove("caption_type"));
  fs::remove(path);
}

// `disable_touch` is overlayable so `just touch-probe` can be run against an INSTALLED build: the
// flag lives in the read-only /app/share/deckback/app.json, and the probe refuses to run with it
// on.
// Without this key, re-asking the P12.4 question costs a full bundle build every time.
void test_disable_touch_is_overlayable_for_the_touch_probe() {
  const std::string path = tmp_path("deckback_user_test_5.json");
  fs::remove(path);
  {
    ConfigStore s(path);
    s.load();
    assert(s.set_bool("disable_touch", false));
  }
  ConfigStore s2(path);
  s2.load();
  Config c;
  assert(c.disable_touch == true);  // the shipped default: touch inert
  s2.apply(c);
  assert(c.disable_touch == false);

  // And it must survive the trip through a hand-written file, which is how the probe sets it.
  write_file(path, R"({"disable_touch": false})");
  ConfigStore s3(path);
  s3.load();
  Config c3;
  s3.apply(c3);
  assert(c3.disable_touch == false);

  // The gesture router rides the same overlay, and for the same reason: turning it on to test it
  // must not require building and side-loading a bundle. The two are mutually exclusive, which
  // main() resolves -- the store's job is only to carry what the user asked for.
  write_file(path, R"({"disable_touch": false, "touch_gestures": true})");
  ConfigStore s5(path);
  s5.load();
  Config c5;
  assert(c5.touch_gestures == false);  // shipped default: off
  s5.apply(c5);
  assert(c5.touch_gestures == true && c5.disable_touch == false);

  // A non-bool must not silently read as false and leave touch live: the default stands.
  write_file(path, R"({"disable_touch": "false"})");
  ConfigStore s4(path);
  s4.load();
  Config c4;
  s4.apply(c4);
  assert(c4.disable_touch == true);
  fs::remove(path);
}

void test_corrupt_overlay_is_ignored_not_fatal() {
  const std::string path = tmp_path("deckback_user_test_4.json");
  write_file(path, "{ this is not json ");
  ConfigStore s(path);
  s.load();
  Config c;
  s.apply(c);
  assert(c.caption_type == "author_first");
  fs::remove(path);
}

}  // namespace

DECKBACK_TEST_MAIN(config_store) {
  test_dump_round_trips_through_parse();
  test_dump_escapes_and_keeps_integers();
  test_set_persist_reload_apply();
  test_non_overlayable_keys_are_refused();
  test_remove_resets_to_default();
  test_disable_touch_is_overlayable_for_the_touch_probe();
  test_corrupt_overlay_is_ignored_not_fatal();
  std::puts("config_store_test: all assertions passed");
  return 0;
}
