// Combinatorial / fuzz coverage of the caption settings state machine (caption_settings.cpp) and
// its persistence (config_store.cpp), including simulated app RESTARTS — so every settings/state
// scenario is exercised off-device before a hardware test.
//
// A "restart" = persist edits through the ConfigStore (user.json), then rebuild a fresh Config from
// the SAME shipped defaults + the reloaded overlay, and construct a new CaptionSettings from it —
// exactly what main() does on the next launch.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "caption_settings.hpp"
#include "config.hpp"
#include "config_store.hpp"
#include "harness.hpp"
#include "json.hpp"

using namespace deckback;

namespace {

namespace fs = std::filesystem;

std::string tmp_path(int n) {
  return (fs::temp_directory_path() / ("deckback_cc_fuzz_" + std::to_string(n) + ".json")).string();
}

bool has(const std::string& hay, std::string_view needle) {
  return hay.find(needle) != std::string::npos;
}

const CaptionRow* find_row(const std::vector<CaptionRow>& rows, std::string_view key) {
  for (const CaptionRow& r : rows)
    if (r.key == key) return &r;
  return nullptr;
}

std::string combo_value(const CaptionSettings& cs, std::string_view key) {
  const auto rows = cs.osd_rows();
  const CaptionRow* r = find_row(rows, key);
  return r ? r->value : std::string();
}

std::vector<std::string> langs_of(const CaptionSettings& cs) {
  std::vector<std::string> out;
  const auto rows = cs.osd_rows();
  const CaptionRow* r = find_row(rows, "languages");
  if (r)
    for (const CaptionOption& it : r->items) out.push_back(it.value);
  return out;
}

bool on_state(const CaptionSettings& cs) { return has(cs.apply_js(), R"("on":true)"); }

void check_invariants(const CaptionSettings& cs) {
  const std::string tj = cs.toggle_js();
  assert(has(tj, R"("mode":"local")") || has(tj, R"("mode":"youtube")"));
  assert(has(tj, R"("op":"toggle")"));
  const std::string ctl = combo_value(cs, "control");
  assert(ctl == "local" || ctl == "youtube");
  assert(cs.local_mode() == (ctl == "local"));
  const std::string ty = combo_value(cs, "type");
  bool type_ok = false;
  for (const CaptionOption& o : caption_type_options())
    if (o.value == ty) type_ok = true;
  assert(type_ok);
  assert(!langs_of(cs).empty());
  json::ParseResult pr = json::parse(cs.osd_model_json());
  assert(pr.ok() && pr.value->is_object());
  assert(!cs.apply_js().empty() && !tj.empty());
}

std::string system_lang_of(const Config&) { return "en"; }

CaptionSettings restart(const std::string& path, const Config& base) {
  ConfigStore store(path);
  store.load();
  Config c = base;
  store.apply(c);
  return CaptionSettings(nullptr, c.caption_control, c.caption_languages, c.caption_type,
                         c.caption_remember, c.captions_toast, c.caption_on, system_lang_of(base));
}

void test_all_combos_persist_across_restart() {
  const std::vector<std::string> controls = {"local", "youtube"};
  const std::vector<std::string> types = {"author_first", "auto_first", "author_only", "auto_only"};
  const std::vector<std::vector<std::string>> lang_sets = {
      {"system"}, {"en"}, {"system", "en"}, {"uk", "en", "system"}, {"fr"}};
  const std::vector<bool> bools = {true, false};

  int n = 0;
  for (const std::string& control : controls)
    for (const std::string& type : types)
      for (const std::vector<std::string>& langs : lang_sets)
        for (bool remember : bools)
          for (bool toast : bools)
            for (bool on : bools) {
              const std::string path = tmp_path(n++ % 8);
              fs::remove(path);

              Config base;
              base.caption_control = control;
              base.caption_type = type;
              base.caption_languages = langs;
              base.caption_remember = remember;
              base.captions_toast = toast;
              base.caption_on = on;

              ConfigStore store(path);
              store.load();
              CaptionSettings cs(&store, control, langs, type, remember, toast, on,
                                 system_lang_of(base));
              check_invariants(cs);

              const std::string target_control = (control == "local") ? "youtube" : "local";
              cs.apply_action("cc.control=" + target_control);
              cs.apply_action("cc.type=auto_only");
              cs.apply_action("cc.remember=on");
              cs.apply_action("cc.toast=off");
              cs.apply_action("cc.lang.add=de");
              cs.set_on(true);
              check_invariants(cs);

              CaptionSettings after = restart(path, base);
              check_invariants(after);
              assert(after.local_mode() == (target_control == "local"));
              assert(combo_value(after, "type") == "auto_only");
              assert(combo_value(after, "remember") == "on");
              assert(combo_value(after, "toast") == "off");
              const auto al = langs_of(after);
              assert(std::find(al.begin(), al.end(), "de") != al.end());
              assert(on_state(after) == true);
              fs::remove(path);
            }
}

void test_on_state_persistence_gated_by_remember() {
  for (bool remember : {true, false}) {
    const std::string path = tmp_path(0);
    fs::remove(path);
    Config base;
    base.caption_remember = remember;
    {
      ConfigStore store(path);
      store.load();
      CaptionSettings cs(&store, "local", {"system"}, "author_first", remember, true, false, "en");
      cs.set_on(true);
    }
    base.caption_on = false;
    CaptionSettings after = restart(path, base);
    assert(on_state(after) == remember);
    fs::remove(path);
  }
}

void test_state_machine_transitions() {
  const std::string path = tmp_path(0);
  fs::remove(path);
  ConfigStore store(path);
  store.load();
  CaptionSettings cs(&store, "local", {"en"}, "author_first", true, true, false, "en");

  assert(on_state(cs) == false);
  cs.set_on(true);
  assert(on_state(cs) == true);
  cs.note_apply("seed_off");
  assert(on_state(cs) == false);
  cs.note_apply("seed_on:fr");
  assert(on_state(cs) == true);
  cs.note_apply("on:fr");
  assert(on_state(cs) == true);
  cs.note_apply("wait");
  cs.note_apply("na");
  cs.note_apply("none");
  assert(on_state(cs) == true);
  check_invariants(cs);
  fs::remove(path);
}

struct Rng {
  uint64_t s;
  uint32_t next() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<uint32_t>(s >> 33);
  }
  uint32_t below(uint32_t n) { return next() % n; }
};

void test_operation_sequence_fuzz() {
  const char* langs[] = {"en", "de", "fr", "uk", "es", "system", "ja"};
  const char* types[] = {"author_first", "auto_first", "author_only", "auto_only"};
  const char* onoff[] = {"on", "off"};

  for (uint64_t seed = 1; seed <= 60; ++seed) {
    Rng rng{seed};
    const std::string path = tmp_path(static_cast<int>(seed % 8));
    fs::remove(path);
    ConfigStore store(path);
    store.load();
    CaptionSettings cs(&store, "local", {"system"}, "author_first", true, true, false, "en");

    for (int step = 0; step < 40; ++step) {
      switch (rng.below(8)) {
        case 0:
          cs.apply_action(std::string("cc.control=") + (rng.below(2) ? "local" : "youtube"));
          break;
        case 1:
          cs.apply_action(std::string("cc.type=") + types[rng.below(4)]);
          break;
        case 2:
          cs.apply_action(std::string("cc.remember=") + onoff[rng.below(2)]);
          break;
        case 3:
          cs.apply_action(std::string("cc.toast=") + onoff[rng.below(2)]);
          break;
        case 4:
          cs.apply_action(std::string("cc.lang.add=") + langs[rng.below(7)]);
          break;
        case 5:
          cs.apply_action(std::string("cc.lang.remove=") + langs[rng.below(7)]);
          break;
        case 6:
          cs.set_on(rng.below(2) != 0);
          break;
        case 7:
          cs.note_apply(rng.below(2) ? "seed_on:fr" : "seed_off");
          break;
      }
      check_invariants(cs);
    }

    Config base;
    CaptionSettings after = restart(path, base);
    check_invariants(after);
    fs::remove(path);
  }
}

void test_adversarial_inputs() {
  const std::string path = tmp_path(0);
  fs::remove(path);
  ConfigStore store(path);
  store.load();
  CaptionSettings cs(&store, "nonsense", {}, "not_a_type", true, true, true, "");
  check_invariants(cs);
  assert(cs.local_mode());

  const char* junk[] = {
      "cc.control=banana", "cc.type=", "cc.lang.add=", "cc.lang.remove=zz", "cc.unknown=1",
      "not.a.cc.action",   "",         "cc.",          "cc.remember=maybe"};
  for (const char* a : junk) {
    cs.apply_action(a);
    check_invariants(cs);
  }
  {
    std::ofstream f(path, std::ios::binary);
    f << R"({"user_agent":"x","caption_control":"youtube","caption_type":"auto_first","caption_languages":["de","fr"],"caption_on":true})";
  }
  Config base;
  CaptionSettings after = restart(path, base);
  check_invariants(after);
  assert(!after.local_mode());
  const auto l = langs_of(after);
  assert(l.size() == 2 && l[0] == "de");
  fs::remove(path);
}

}  // namespace

DECKBACK_TEST_MAIN(caption_fuzz) {
  test_all_combos_persist_across_restart();
  test_on_state_persistence_gated_by_remember();
  test_state_machine_transitions();
  test_operation_sequence_fuzz();
  test_adversarial_inputs();
  std::puts("caption_fuzz_test: all assertions passed");
  return 0;
}
