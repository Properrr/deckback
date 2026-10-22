// The page-script registry: ScriptParams JSON/escaping, render()'s (body)(params) shape, the
// embedded scripts' contents, and runtime overrides from a scripts dir.

#include "scripts.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "harness.hpp"

using namespace deckback;

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

void test_params_empty_is_empty_object() { assert(ScriptParams().json() == "{}"); }

void test_params_numbers_and_bools() {
  assert(ScriptParams().set("delta", -10).json() == R"({"delta":-10})");
  assert(ScriptParams().set("n", 42).json() == R"({"n":42})");
  assert(ScriptParams().set("on", true).json() == R"({"on":true})");
  assert(ScriptParams().set("off", false).json() == R"({"off":false})");
  // Two keys join with a comma, in insertion order.
  assert(ScriptParams().set("a", 1).set("b", 2).json() == R"({"a":1,"b":2})");
}

void test_params_strings_are_escaped_once() {
  // Quotes and backslashes are escaped into a valid JS/JSON string.
  assert(ScriptParams().set("text", std::string_view("say \"hi\"")).json() ==
         R"({"text":"say \"hi\""})");
  assert(ScriptParams().set("p", std::string_view("a\\b")).json() == R"({"p":"a\\b"})");
}

void test_params_string_array() {
  std::vector<std::string> sels{"[aria-label*='voice' i]", ".ytlr__mic"};
  assert(ScriptParams().set("selectors", sels).json() ==
         R"({"selectors":["[aria-label*='voice' i]",".ytlr__mic"]})");
  assert(ScriptParams().set("selectors", std::vector<std::string>{}).json() ==
         R"({"selectors":[]})");
}

void test_params_raw_is_verbatim() {
  assert(ScriptParams().set_raw("cfg", "window.__x").json() == R"({"cfg":window.__x})");
}

void test_render_appends_params_to_the_body() {
  // The embedded skip.js is a function expression; render wraps it into a call with the params.
  const std::string js = ScriptLibrary::instance().render("skip", ScriptParams().set("delta", -10));
  assert(has(js, "seekBy"));  // the real body is present
  assert(has(js, "#movie_player"));
  assert(has(js, R"(({"delta":-10}))"));  // ...invoked with the params object
  // No params -> invoked with {}.
  assert(has(ScriptLibrary::instance().render("toast_hide"), "({})"));
}

void test_render_unknown_is_empty() {
  assert(ScriptLibrary::instance().render("no_such_script").empty());
  assert(ScriptLibrary::instance().body("no_such_script").empty());
}

void test_embedded_defaults_present() {
  // config/scripts/ is embedded with its real content.
  assert(has(std::string(ScriptLibrary::instance().body("av1_steering")), "MediaSource"));
  assert(
      has(std::string(ScriptLibrary::instance().body("no_pointer")), "stopImmediatePropagation"));
  assert(has(std::string(ScriptLibrary::instance().body("toast")), "__deckback_toast"));
  const std::string cc = ScriptLibrary::instance().render("toggle_captions");
  assert(has(cc, "getOption"));
  assert(has(cc, "setOption"));
  assert(has(cc, "tracklist"));
  assert(has(cc, "html5-video-player"));
  assert(has(cc, "({})"));
  const std::string cc_p = ScriptLibrary::instance().render(
      "toggle_captions", ScriptParams()
                             .set("langs", std::vector<std::string>{"en", "de"})
                             .set("type", std::string_view("author_first")));
  assert(has(cc_p, R"("langs":["en","de"])"));
  assert(has(cc_p, R"("type":"author_first")"));
}

void test_overlays_use_a_csp_safe_style_path() {
  // The page's CSP drops inline style attributes and <style> tags; only CSSOM and
  // adoptedStyleSheets work.
  for (const char* name : {"toast", "osd_button"}) {
    const std::string b(ScriptLibrary::instance().body(name));
    assert(!has(b, "setAttribute('style'"));
    assert(!has(b, "setAttribute(\"style\""));
    assert(has(b, ".setProperty("));
  }
  // The OSD's descendant rules use adoptedStyleSheets, with a CSSOM fallback on the container.
  {
    const std::string b(ScriptLibrary::instance().body("osd"));
    assert(!has(b, "<style>"));
    assert(has(b, "adoptedStyleSheets"));
    assert(has(b, ".setProperty("));
  }
}

void test_osd_exit_is_a_global_hold_not_a_focusable_control() {
  // Exit is a global hold from any tab and focus, not a focusable control (the Keys sub-tab never
  // lets focus leave its list).
  const std::string b(ScriptLibrary::instance().body("osd"));
  assert(has(b, "'exit'"));
  assert(has(b, "hold_start"));
  assert(has(b, "hold_cancel"));
  assert(has(b, "return 'hold'"));
  assert(!has(b, "focusables.push(S.exitBar)"));
}

void test_osd_updates_tab_scrolls_the_release_notes() {
  // On the Updates tab ↑/↓ scroll the release notes and ←/→ move between the buttons.
  const std::string b(ScriptLibrary::instance().body("osd"));
  assert(has(b, "S.notesBox()"));
  assert(has(b, "if (S.notesBox()) return S.scroll(dir);"));
  assert(has(b, "if (S.tab === 'updates') return S.move(dir);"));
  // The hint bar says so.
  assert(has(b, "NAV_HINT_NOTES"));
  assert(has(b, "Scroll notes"));
}

void test_toast_wraps_instead_of_clipping() {
  // pre-wrap plus a max-width keeps a long, centred toast on screen.
  const std::string b(ScriptLibrary::instance().body("toast"));
  assert(has(b, "'pre-wrap'"));
  assert(!has(b, "'white-space', 'pre'"));
  assert(has(b, "max-width"));
}

void test_overlays_self_heal_across_body_swaps() {
  // A keep-alive observer re-appends overlays a body swap detaches; the *_hide scripts drop out of
  // its registry first so a deliberate hide sticks.
  for (const char* name : {"osd", "osd_button"}) {
    const std::string b(ScriptLibrary::instance().body(name));
    assert(has(b, "__dbKeepAlive"));
    assert(has(b, "MutationObserver"));
  }
  for (const char* name : {"osd_button_hide"}) {
    const std::string b(ScriptLibrary::instance().body(name));
    assert(has(b, "__dbDropAlive"));
  }
}

void test_runtime_override_shadows_the_default() {
  namespace fs = std::filesystem;
  const fs::path dir = deckback_test_tmpdir() / "deckback_scripts_override_test";
  fs::create_directories(dir);
  {
    std::ofstream f(dir / "skip.js");
    f << "(function(p){return 'OVERRIDDEN:'+p.delta;})";
  }
  // A blank override file is ignored.
  {
    std::ofstream f(dir / "toast.js");
    f << "   \n\t ";
  }

  ScriptLibrary::instance().load_overrides(dir.string());
  assert(has(std::string(ScriptLibrary::instance().body("skip")), "OVERRIDDEN"));
  assert(has(ScriptLibrary::instance().render("skip", ScriptParams().set("delta", 5)),
             R"(OVERRIDDEN:'+p.delta;})({"delta":5}))"));
  // toast kept its embedded default despite the blank override file.
  assert(has(std::string(ScriptLibrary::instance().body("toast")), "__deckback_toast"));

  std::error_code ec;
  fs::remove_all(dir, ec);
}

DECKBACK_TEST_MAIN(scripts) {
  test_params_empty_is_empty_object();
  test_params_numbers_and_bools();
  test_params_strings_are_escaped_once();
  test_params_string_array();
  test_params_raw_is_verbatim();
  test_render_appends_params_to_the_body();
  test_render_unknown_is_empty();
  test_embedded_defaults_present();
  test_overlays_use_a_csp_safe_style_path();
  test_toast_wraps_instead_of_clipping();
  test_osd_exit_is_a_global_hold_not_a_focusable_control();
  test_osd_updates_tab_scrolls_the_release_notes();
  test_overlays_self_heal_across_body_swaps();
  // Last: it mutates the process-wide singleton.
  test_runtime_override_shadows_the_default();
  std::puts("scripts_test: all passed");
  return 0;
}
