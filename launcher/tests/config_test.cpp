// Config::load — the JSON-shape contract, the validation, and the failure modes.
//
// Before the parser rewrite this file covered exactly one happy path and one missing file. It had
// no malformed-input case at all, which is why a corrupt app.json silently loading as all-defaults
// went unnoticed. app.json is HOT-SWAPPABLE: a half-written file is a normal event on that path.
#include "config.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

#include "harness.hpp"

using deckback::Config;
using deckback::SelfUpdateMode;

namespace {

const char* kPath = "config_test_tmp.json";

std::optional<Config> load(std::string_view json) {
  {
    std::ofstream f(kPath);
    f << json;
  }
  auto c = Config::load(kPath);
  std::remove(kPath);
  return c;
}

void test_happy_path() {
  auto c = load(R"({
      "url": "https://www.youtube.com/tv",
      "user_agent": "TestUA/1.0",
      "remote_debugging_port": 9222,
      "watchdog": { "restart_on_crash": false, "max_restarts_per_minute": 3 },
      "cobalt_flags": ["--ozone-platform=x11", "--force-device-scale-factor=1"]
    })");
  assert(c.has_value());
  assert(c->url == "https://www.youtube.com/tv");
  assert(c->user_agent == "TestUA/1.0");
  assert(c->remote_debugging_port == 9222);
  assert(c->watchdog_restart_on_crash == false);
  assert(c->watchdog_max_restarts_per_minute == 3);
  assert(c->cobalt_flags.size() == 2);
  assert(c->cobalt_flags[0] == "--ozone-platform=x11");
}

void test_missing_file_is_an_error() { assert(!Config::load("does_not_exist.json").has_value()); }

// ---- failure is representable --------------------------------------------------------------
//
// THE regression. Config::load's only nullopt path used to be "cannot read the file", so every one
// of these loaded "successfully" into all-defaults — including remote_debugging_port 0, which
// disables the navigator, gamepad, OSD and onboarding. A controller-less content_shell, silently.

void test_malformed_config_is_refused_not_defaulted() {
  assert(!load("").has_value());
  assert(!load("{").has_value());
  assert(!load(R"({"url": })").has_value());
  assert(!load(R"({"url":"https://www.youtube.com/tv","remote_debugging_p)").has_value());  // cut
  assert(!load("<!DOCTYPE html><h1>502 Bad Gateway</h1>").has_value());   // fetched an error page
  assert(!load(std::string("\x7f\x45\x4c\x46\x02\x01", 6)).has_value());  // binary
  assert(!load("[1,2,3]").has_value());                                   // not an object
  assert(!load("\"just a string\"").has_value());
}

// ---- the nesting is real now ----------------------------------------------------------------
//
// app.json nests (power.*, log.*, quality.*, watchdog.*). The old extractor found those leaves by
// searching the raw bytes for the name ANYWHERE, so nesting was decorative and a nested key
// silently shadowed a top-level one. Paths are bound exactly now.

void test_nested_sections_bind_by_path() {
  auto c = load(R"({
      "power": { "devtools_poll_ms": 500, "resume_probe_host": "example.com",
                 "resume_probe_port": 8443 },
      "log": { "log_max_bytes": 123456, "log_max_files": 7, "log_mirror_stderr": false },
      "quality": { "steer_av1_unsupported": false },
      "watchdog": { "max_restarts_per_minute": 9 }
    })");
  assert(c.has_value());
  assert(c->devtools_poll_ms == 500);
  assert(c->resume_probe_host == "example.com");
  assert(c->resume_probe_port == 8443);
  assert(c->log_max_bytes == 123456);
  assert(c->log_max_files == 7);
  assert(c->log_to_stderr == false);
  assert(c->steer_av1 == false);
  assert(c->watchdog_max_restarts_per_minute == 9);
}

void test_nested_key_no_longer_shadows_a_top_level_one() {
  // `{"keymap":{"url":...},"url":...}` used to resolve `url` to the NESTED value — the launcher
  // would navigate somewhere else entirely.
  auto c = load(R"({"keymap":{"url":"https://SHADOWED/"},"url":"https://www.youtube.com/tv"})");
  assert(c.has_value());
  assert(c->url == "https://www.youtube.com/tv");
}

// ---- typed reads distinguish absent from wrong ------------------------------------------------

void test_wrong_type_keeps_the_default_and_says_so() {
  // A quoted port used to leave remote_debugging_port at 0 silently — the whole app degraded from
  // one pair of quotes. It still keeps the default, but now warns (see the WARN in the log).
  auto c = load(R"({"remote_debugging_port":"9222"})");
  assert(c.has_value());
  assert(c->remote_debugging_port == 0);

  auto b = load(R"({"disable_touch":"true"})");
  assert(b.has_value());
  assert(b->disable_touch == true);  // the default, not the string
}

void test_bool_is_not_prefix_matched() {
  // read_bool did `compare(p, 4, "true")`, so `truthy` was true. It is now a parse error.
  assert(!load(R"({"error_page": truthy})").has_value());
}

void test_exponent_numbers_are_read_whole() {
  // std::stol over the rest of the file read `5e6` as 5 — log_max_bytes off by 10^6.
  auto c = load(R"({"log":{"log_max_bytes": 5e6}})");
  assert(c.has_value());
  assert(c->log_max_bytes == 5000000);
}

void test_string_escapes_survive() {
  // error_title/error_hint are the designated R1 config-push surface, and could not express a
  // newline: "a\nb" arrived as "anb".
  auto c = load(R"({"error_title":"Can’t reach YouTube","error_hint":"One.\nTwo.\tTabbed."})");
  assert(c.has_value());
  assert(c->error_title == "Can’t reach YouTube");
  assert(c->error_hint == "One.\nTwo.\tTabbed.");
}

void test_array_with_a_bracket_inside_a_string() {
  // read_string_array ended the array at the first `]` BYTE. This is the shape that broke
  // voice_mic_selectors in the shipped config.
  auto c = load(R"({"cobalt_flags":["--a=[x]","--b","--c=]"]})");
  assert(c.has_value());
  assert(c->cobalt_flags.size() == 3);
  assert(c->cobalt_flags[0] == "--a=[x]");
  assert(c->cobalt_flags[2] == "--c=]");
}

// ---- validation --------------------------------------------------------------------------------

void test_out_of_range_values_are_clamped() {
  // There was no range validation at all: devtools_poll_ms 0 meant a busy-loop poll.
  auto c = load(R"({"power":{"devtools_poll_ms":0},"skip_seconds":100000,
                    "right_stick_deadzone":-5})");
  assert(c.has_value());
  assert(c->devtools_poll_ms == 100);    // clamped up
  assert(c->skip_seconds == 600);        // clamped down
  assert(c->right_stick_deadzone == 1);  // clamped up
}

void test_integer_settings_reject_fractional_values_and_clamp_huge_ones() {
  auto fractional = load(R"({"skip_seconds":1.5,"remote_debugging_port":9222.5})");
  assert(fractional.has_value());
  assert(fractional->skip_seconds == 10);
  assert(fractional->remote_debugging_port == 0);

  auto huge = load(R"({"remote_debugging_port":1e100})");
  assert(huge.has_value());
  assert(huge->remote_debugging_port == 65535);
}

void test_inverted_ranges_are_swapped() {
  auto c = load(R"({"error_retry_min_ms": 30000, "error_retry_max_ms": 2000})");
  assert(c.has_value());
  assert(c->error_retry_min_ms == 2000);
  assert(c->error_retry_max_ms == 30000);
}

void test_keymap_order_is_preserved_and_comments_skipped() {
  auto c = load(R"({"keymap":{"$comment":"doc","b":"back","a":"select","x":"playpause"}})");
  assert(c.has_value());
  assert(c->keymap.size() == 3);
  assert(c->keymap[0].first == "b");  // written order, not sorted
  assert(c->keymap[1].first == "a");
  assert(c->keymap[2].first == "x");
}

void test_self_update_modes() {
  assert(load(R"({"power":{"self_update_mode":"auto"}})")->self_update_mode ==
         SelfUpdateMode::Auto);
  assert(load(R"({"power":{"self_update_mode":"off"}})")->self_update_mode == SelfUpdateMode::Off);
  assert(load(R"({"self_update_mode":"auto"})")->self_update_mode == SelfUpdateMode::Auto);
  // Unrecognised -> notify: never deploy without consent.
  assert(load(R"({"self_update_mode":"aut"})")->self_update_mode == SelfUpdateMode::Notify);
  // The legacy boolean still maps.
  assert(load(R"({"self_update":true})")->self_update_mode == SelfUpdateMode::Auto);
  assert(load(R"({"self_update":false})")->self_update_mode == SelfUpdateMode::Off);
  // Absent -> the safe default.
  assert(load("{}")->self_update_mode == SelfUpdateMode::Notify);
}

void test_schema_version() {
  assert(load(R"({"schema_version":1})").has_value());
  // A file written for a newer launcher may rely on settings this build cannot honour; refusing
  // beats half-applying an emergency config push.
  assert(!load(R"({"schema_version":99})").has_value());
  assert(!load(R"({"schema_version":"1"})").has_value());
  assert(!load(R"({"schema_version":1.5})").has_value());
  assert(!load(R"({"schema_version":-1})").has_value());
  assert(!load(R"({"schema_version":1e100})").has_value());
}

void test_empty_object_is_all_defaults() {
  // Valid but empty: defaults, and NOT an error — distinct from malformed.
  auto c = load("{}");
  assert(c.has_value());
  assert(c->url == "https://www.youtube.com/tv");
  assert(c->disable_touch == true);
}

}  // namespace

DECKBACK_TEST_MAIN(config) {
  test_happy_path();
  test_missing_file_is_an_error();
  test_malformed_config_is_refused_not_defaulted();
  test_nested_sections_bind_by_path();
  test_nested_key_no_longer_shadows_a_top_level_one();
  test_wrong_type_keeps_the_default_and_says_so();
  test_bool_is_not_prefix_matched();
  test_exponent_numbers_are_read_whole();
  test_string_escapes_survive();
  test_array_with_a_bracket_inside_a_string();
  test_out_of_range_values_are_clamped();
  test_integer_settings_reject_fractional_values_and_clamp_huge_ones();
  test_inverted_ranges_are_swapped();
  test_keymap_order_is_preserved_and_comments_skipped();
  test_self_update_modes();
  test_schema_version();
  test_empty_object_is_all_defaults();
  std::puts("config_test: ok");
  return 0;
}
