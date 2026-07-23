// The page-script registry (scripts.cpp). Pure surface: ScriptParams JSON/escaping, render()'s
// (body)(params) shape against the embedded defaults, and runtime override from a scripts dir. The
// CDP dispatch (invoke/install_sticky) is one line over DevToolsClient and is exercised through the
// feature tests (overlay_test, navigator_test); here we prove the string plumbing.

#include "scripts.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

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
  // Quotes and backslashes must survive into a valid JS/JSON string — the escaping bug this
  // centralises used to be re-derived at every call site.
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
  // The whole config/scripts/ dir is embedded; a couple of known members must be there, with their
  // real content (sticky scripts are self-invoking, not the (function(p) shape).
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
  // youtube.com/tv's CSP style-src has no 'unsafe-inline', so setAttribute('style',…) and <style>
  // tags are dropped and the overlay renders unstyled (why dot/card/toast failed on-Deck,
  // self-update.md). Only CSSOM and adoptedStyleSheets are exempt; guard against a regression.
  for (const char* name : {"toast", "osd_button"}) {
    const std::string b(ScriptLibrary::instance().body(name));
    assert(!has(b, "setAttribute('style'"));
    assert(!has(b, "setAttribute(\"style\""));
    assert(has(b, ".setProperty("));
  }
  // A modal overlay's descendant rules need a stylesheet, so the blocked <style> tag becomes an
  // adoptedStyleSheets sheet; the container keeps a CSSOM fallback so it always paints.
  {
    const std::string b(ScriptLibrary::instance().body("osd"));
    assert(!has(b, "<style>"));
    assert(has(b, "adoptedStyleSheets"));
    assert(has(b, ".setProperty("));
  }
}

void test_osd_exit_row_is_hold_to_confirm_on_every_tab() {
  // Exit is chrome, not tab content: collect() appends it after the panel's own focusables, so it
  // is the last stop wherever you are. And A on it must start a hold ('hold') rather than act, or a
  // single stray press ends the session.
  const std::string b(ScriptLibrary::instance().body("osd"));
  assert(has(b, "'exit'"));
  assert(has(b, "S.focusables.push(S.exitBar)"));
  assert(has(b, "holdStart"));
  assert(has(b, "hold_cancel"));
  assert(has(b, "return 'hold'"));
}

void test_toast_wraps_instead_of_clipping() {
  // `white-space: pre` never wraps, so a toast wider than the panel was clipped at BOTH edges (it
  // is centred with translateX(-50%)) — silently, with no ellipsis. Observed on-Deck with the
  // keep-awake warning. pre-wrap + a max-width is what keeps a long or hot-swapped string readable.
  const std::string b(ScriptLibrary::instance().body("toast"));
  assert(has(b, "'pre-wrap'"));
  assert(!has(b, "'white-space', 'pre'"));
  assert(has(b, "max-width"));
}

void test_overlays_self_heal_across_body_swaps() {
  // A Leanback in-page body swap can detach a documentElement child without firing on_app_loaded,
  // so the keep-alive observer re-appends the dot/card; the *_hide scripts must drop from the
  // registry first, or a deliberate hide is fought. A vanished-but-still-modal card was the input
  // trap.
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
  const fs::path dir = fs::temp_directory_path() / "deckback_scripts_override_test";
  fs::create_directories(dir);
  {
    std::ofstream f(dir / "skip.js");
    f << "(function(p){return 'OVERRIDDEN:'+p.delta;})";
  }
  // A blank file must be IGNORED (a truncation must not silently disable the behaviour).
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

int main() {
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
  test_osd_exit_row_is_hold_to_confirm_on_every_tab();
  test_overlays_self_heal_across_body_swaps();
  // Keep this LAST: it mutates the process-wide singleton (a fresh process per test binary, so this
  // is safe, but ordering it last keeps the earlier assertions against pristine embedded defaults).
  test_runtime_override_shadows_the_default();
  std::puts("scripts_test: all passed");
  return 0;
}
