// CaptionSettings (caption_settings.cpp): the live caption-button model shared by the input layer
// and the OSD. Language tables, the rendered toggle params, the OSD rows, and apply_action +
// persistence through a temp ConfigStore.
#include "caption_settings.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "config.hpp"
#include "config_store.hpp"
#include "harness.hpp"

using namespace deckback;

namespace {

namespace fs = std::filesystem;

bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

std::string tmp_path(const char* name) { return (fs::temp_directory_path() / name).string(); }

const CaptionRow* row(const std::vector<CaptionRow>& rows, std::string_view key) {
  for (const CaptionRow& r : rows)
    if (r.key == key) return &r;
  return nullptr;
}

void test_language_tables() {
  assert(language_name("en") == "English");
  assert(language_name("pt") == "Portuguese");
  assert(language_name("zz") == "zz");
  assert(!curated_languages().empty());
  assert(all_languages().size() > curated_languages().size());
  assert(caption_type_options().size() == 4);
}

void test_toggle_and_apply_js() {
  ConfigStore store(tmp_path("deckback_cc_test_js.json"));
  fs::remove(store.path());
  CaptionSettings cs(&store, "local", {"system", "en"}, "auto_first", true, true, false, "en");
  const std::string tj = cs.toggle_js();
  assert(has(tj, R"("op":"toggle")"));
  assert(has(tj, R"("mode":"local")"));
  assert(has(tj, R"("langs":["en"])"));
  assert(has(tj, R"("type":"auto_first")"));
  assert(has(tj, R"("sys":"en")"));
  const std::string aj = cs.apply_js();
  assert(has(aj, R"("op":"apply")"));
  assert(has(aj, R"("on":false)"));
  assert(has(aj, R"("known":true)"));
  fs::remove(store.path());
}

void test_youtube_mode() {
  ConfigStore store(tmp_path("deckback_cc_test_yt.json"));
  fs::remove(store.path());
  CaptionSettings cs(&store, "youtube", {"system"}, "author_first", true, true, false, "en");
  assert(!cs.local_mode());
  assert(has(cs.toggle_js(), R"("mode":"youtube")"));
  const auto rows = cs.osd_rows();
  assert(rows[0].key == "control" && rows[0].value == "youtube");
  fs::remove(store.path());
}

void test_osd_rows_reflect_state() {
  ConfigStore store(tmp_path("deckback_cc_test_rows.json"));
  fs::remove(store.path());
  CaptionSettings cs(&store, "local", {"system", "de"}, "author_first", true, false, false, "en");
  const auto rows = cs.osd_rows();
  const CaptionRow* langs = row(rows, "languages");
  assert(langs && langs->kind == "langlist");
  assert(langs->items.size() == 2);
  assert(langs->items[0].value == "system" && has(langs->items[0].label, "English"));
  assert(langs->items[1].value == "de" && langs->items[1].label == "German");
  const CaptionRow* type = row(rows, "type");
  assert(type && type->kind == "combo" && type->value == "author_first" &&
         type->options.size() == 4);
  assert(row(rows, "toast")->value == "off");
  fs::remove(store.path());
}

void test_apply_action_and_persist() {
  const std::string path = tmp_path("deckback_cc_test_apply.json");
  fs::remove(path);
  ConfigStore store(path);
  store.load();
  CaptionSettings cs(&store, "local", {"system"}, "author_first", true, true, false, "en");

  assert(cs.apply_action("cc.type=auto_first"));
  assert(!cs.apply_action("cc.type=auto_first"));
  assert(!cs.apply_action("cc.type=nonsense"));
  assert(cs.apply_action("cc.lang.add=de"));
  assert(!cs.apply_action("cc.lang.add=de"));
  assert(cs.apply_action("cc.remember=off"));
  assert(cs.apply_action("cc.toast=off"));

  ConfigStore reload(path);
  reload.load();
  Config c;
  reload.apply(c);
  assert(c.caption_type == "auto_first");
  assert(c.caption_remember == false);
  assert(c.captions_toast == false);
  assert(c.caption_languages.size() == 2 && c.caption_languages[1] == "de");
  fs::remove(path);
}

void test_remove_never_empties_the_list() {
  ConfigStore store(tmp_path("deckback_cc_test_rm.json"));
  fs::remove(store.path());
  CaptionSettings cs(&store, "local", {"system"}, "author_first", true, true, false, "en");
  assert(cs.apply_action("cc.lang.remove=system"));
  const auto rows = cs.osd_rows();
  assert(row(rows, "languages")->items.size() == 1);
  assert(row(rows, "languages")->items[0].value == "system");
  fs::remove(store.path());
}

void test_language_reorder() {
  const std::string path = tmp_path("deckback_cc_test_reorder.json");
  fs::remove(path);
  ConfigStore store(path);
  store.load();
  CaptionSettings cs(&store, "local", {"uk", "system", "de"}, "author_first", true, true, false,
                     "en");

  assert(cs.apply_action("cc.lang.up=de"));
  {
    const auto rows = cs.osd_rows();
    const CaptionRow* l = row(rows, "languages");
    assert(l->items.size() == 3);
    assert(l->items[0].value == "uk" && l->items[1].value == "de" && l->items[2].value == "system");
  }
  assert(!cs.apply_action("cc.lang.up=uk"));
  assert(!cs.apply_action("cc.lang.down=system"));
  assert(!cs.apply_action("cc.lang.up=zz"));
  assert(cs.apply_action("cc.lang.down=uk"));

  ConfigStore reload(path);
  reload.load();
  Config c;
  reload.apply(c);
  assert(c.caption_languages.size() == 3);
  assert(c.caption_languages[0] == "de" && c.caption_languages[1] == "uk" &&
         c.caption_languages[2] == "system");
  fs::remove(path);
}

void test_picker_starts_with_system() {
  ConfigStore store(tmp_path("deckback_cc_test_pick.json"));
  fs::remove(store.path());
  CaptionSettings cs(&store, "local", {"system"}, "author_first", true, true, false, "en");
  const auto picks = cs.picker_languages();
  assert(!picks.empty());
  assert(picks[0].value == "system");
  for (size_t i = 0; i < picks.size(); ++i)
    for (size_t j = i + 1; j < picks.size(); ++j) assert(picks[i].value != picks[j].value);
  fs::remove(store.path());
}

void test_control_and_on_state() {
  const std::string path = tmp_path("deckback_cc_test_ctrl.json");
  fs::remove(path);
  ConfigStore store(path);
  store.load();
  CaptionSettings cs(&store, "local", {"system"}, "author_first", true, true, false, "en");
  assert(cs.local_mode());
  assert(cs.apply_action("cc.control=youtube"));
  assert(!cs.local_mode());
  assert(!cs.apply_action("cc.control=youtube"));
  assert(cs.apply_action("cc.control=local"));

  cs.set_on(true);
  cs.note_apply("seed_off");
  {
    ConfigStore reload(path);
    reload.load();
    Config c;
    reload.apply(c);
    assert(c.caption_control == "local");
    assert(c.caption_on == false);
  }
  fs::remove(path);

  ConfigStore s2(path);
  s2.load();
  CaptionSettings cs2(&store, "local", {"system"}, "author_first", false, true, false, "en");
  cs2.set_on(true);
  assert(!s2.has("caption_on"));
  fs::remove(path);
}

}  // namespace

DECKBACK_TEST_MAIN(caption_settings) {
  test_language_tables();
  test_toggle_and_apply_js();
  test_youtube_mode();
  test_osd_rows_reflect_state();
  test_apply_action_and_persist();
  test_remove_never_empties_the_list();
  test_language_reorder();
  test_picker_starts_with_system();
  test_control_and_on_state();
  std::puts("caption_settings_test: all assertions passed");
  return 0;
}
