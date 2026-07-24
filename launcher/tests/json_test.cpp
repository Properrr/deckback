// Unit tests for the JSON parser that replaced config.cpp's byte-scanning extractor.
//
// Every "regression" case below is a defect the extractor actually had, verified against the real
// Config::load before this parser existed. They are not hypothetical.
#include "json.hpp"

#include <cassert>
#include <cstdio>
#include <string>

#include "harness.hpp"

using namespace deckback;
using namespace deckback::json;

namespace {

Value must_parse(std::string_view text) {
  ParseResult r = parse(text);
  if (!r.ok()) std::fprintf(stderr, "unexpected parse error: %s\n", r.error.message.c_str());
  assert(r.ok());
  return std::move(*r.value);
}

void must_fail(std::string_view text) {
  ParseResult r = parse(text);
  assert(!r.ok());
  assert(!r.error.message.empty());  // a failure that cannot say why is barely a failure
}

// ---- scalars ------------------------------------------------------------------------------------

void test_scalars() {
  assert(must_parse("true").as_bool() == true);
  assert(must_parse("false").as_bool() == false);
  assert(must_parse("null").is_null());
  assert(must_parse("42").as_number() == 42.0);
  assert(must_parse("-7").as_number() == -7.0);
  assert(must_parse("1.5").as_number() == 1.5);
  assert(*must_parse("\"hi\"").as_string() == "hi");
}

// The extractor's read_bool did `s.compare(p, 4, "true")`, so any value STARTING with "true"
// matched. `truthy` became true.
void test_regression_bool_is_not_prefix_matched() {
  must_fail("truthy");
  must_fail("{\"a\": truthy}");
  must_fail("falsey");
  // A quoted "true" is a string, not a bool -- and now the caller can tell.
  Value v = must_parse("{\"a\": \"true\"}");
  assert(v.find("a")->as_bool() == std::nullopt);
  assert(*v.find("a")->as_string() == "true");
}

// The extractor ran std::stol over the REST OF THE FILE, which parses a leading integer and ignores
// the rest: 5e6 became 5 (log_max_bytes off by a factor of a million) and 1.5e3 became 1.
void test_regression_exponent_numbers() {
  assert(must_parse("5e6").as_number() == 5000000.0);
  assert(must_parse("1.5e3").as_number() == 1500.0);
  assert(must_parse("-2E2").as_number() == -200.0);
  // Malformed numbers are now errors rather than a silently truncated prefix.
  must_fail("5e");
  must_fail("01");  // JSON forbids leading zeros
  must_fail(".5");
  must_fail("1.");
  must_fail("+1");
}

// The extractor dropped the escape MARKER and kept the next raw byte: "a\nb" -> "anb",
// "Can’t" -> "Cantu2019t". error_title/error_hint are the designated R1 config-push surface,
// so this corrupted exactly the text meant to be hot-fixable.
void test_regression_string_escapes() {
  assert(*must_parse(R"("Line one.\nLine two.")").as_string() == "Line one.\nLine two.");
  assert(*must_parse(R"("a\tb")").as_string() == "a\tb");
  assert(*must_parse(R"("a\r\nb")").as_string() == "a\r\nb");
  assert(*must_parse(R"("say \"hi\"")").as_string() == "say \"hi\"");
  assert(*must_parse(R"("back\\slash")").as_string() == "back\\slash");
  assert(*must_parse(R"("a\/b")").as_string() == "a/b");
  assert(*must_parse(R"("\b\f")").as_string() == "\b\f");
}

void test_unicode_escapes() {
  assert(*must_parse(R"("Can’t")").as_string() == "Can’t");  // U+2019, 3-byte UTF-8
  assert(*must_parse(R"("A")").as_string() == "A");
  assert(*must_parse(R"("é")").as_string() == "é");  // 2-byte
  // Surrogate pair -> one 4-byte code point (U+1F600).
  assert(*must_parse(R"("😀")").as_string() == "\U0001F600");
  must_fail(R"("\u12")");
  must_fail(R"("\uZZZZ")");
  must_fail(R"("\q")");
}

// ---- arrays -------------------------------------------------------------------------------------

// THE headline regression. read_string_array ended the array at the first `]` BYTE, which in the
// shipped app.json fell INSIDE the first string ("[aria-label*='voice' i]"), yielding one truncated
// selector instead of five.
void test_regression_bracket_inside_a_string_does_not_end_the_array() {
  Value v = must_parse(R"({"sel": ["[aria-label*='voice' i]", "[class*='mic'][role='button']",
                                   ".plain", "a]b", "c[d"]})");
  const auto* a = v.find("sel")->as_array();
  assert(a != nullptr);
  assert(a->size() == 5);
  assert(*(*a)[0].as_string() == "[aria-label*='voice' i]");
  assert(*(*a)[1].as_string() == "[class*='mic'][role='button']");
  assert(*(*a)[2].as_string() == ".plain");
  assert(*(*a)[3].as_string() == "a]b");
  assert(*(*a)[4].as_string() == "c[d");
}

void test_arrays() {
  assert(must_parse("[]").as_array()->empty());
  assert(must_parse("[1,2,3]").as_array()->size() == 3);
  assert(must_parse(R"([[1],[2,[3]]])").as_array()->size() == 2);
  must_fail("[1,2");
  must_fail("[1,,2]");
  must_fail("[1,2,]");  // no trailing commas in JSON
}

// ---- objects and paths --------------------------------------------------------------------------

// The extractor searched the whole file for "\"key\"", so a key nested anywhere shadowed a
// top-level key of the same name. `{"keymap":{"url":"..."},"url":"..."}` resolved to the NESTED
// url, and the launcher would navigate there.
void test_regression_nested_key_does_not_shadow_top_level() {
  Value v = must_parse(R"({"keymap":{"url":"https://SHADOWED/"},"url":"https://real/"})");
  assert(*v.find("url")->as_string() == "https://real/");
  assert(*v.find("keymap.url")->as_string() == "https://SHADOWED/");
}

// The extractor's brace matcher ignored strings, so a `{` or `}` inside a value broke it: a `}`
// truncated the object, a `{` made it return an EMPTY keymap -- every binding silently gone.
void test_regression_braces_inside_strings() {
  Value v = must_parse(R"({"keymap":{"a":"}","b":"{"},"url":"https://real/"})");
  assert(*v.find("keymap.a")->as_string() == "}");
  assert(*v.find("keymap.b")->as_string() == "{");
  assert(*v.find("url")->as_string() == "https://real/");
  assert(v.find("keymap")->as_object()->size() == 2);
}

void test_paths() {
  Value v = must_parse(R"({"a":{"b":{"c":1}},"d":2})");
  assert(v.find("a.b.c")->as_number() == 1.0);
  assert(v.find("d")->as_number() == 2.0);
  assert(v.find("a.b") != nullptr);
  assert(v.find("nope") == nullptr);
  assert(v.find("a.b.c.d") == nullptr);  // traversing through a non-object
  assert(v.find("a.nope") == nullptr);
  assert(v.find("") == nullptr);
}

// keymap order drives the startup log and the first-run card, so it must survive parsing.
void test_object_order_is_preserved() {
  Value v = must_parse(R"({"z":1,"a":2,"m":3})");
  const auto* o = v.as_object();
  assert(o->size() == 3);
  assert((*o)[0].first == "z" && (*o)[1].first == "a" && (*o)[2].first == "m");
}

// The extractor kept the FIRST duplicate; every mainstream parser keeps the last.
void test_duplicate_keys_last_wins() {
  Value v = must_parse(R"({"url":"https://a","url":"https://b"})");
  assert(*v.find("url")->as_string() == "https://b");
  assert(v.as_object()->size() == 1);
}

void test_leaf_paths() {
  Value v = must_parse(R"({"a":1,"b":{"c":2,"d":{"e":3}},"f":[1,2]})");
  const auto p = v.leaf_paths();
  assert(p.size() == 4);
  assert(p[0] == "a");
  assert(p[1] == "b.c");
  assert(p[2] == "b.d.e");
  assert(p[3] == "f");  // an array is a leaf
}

// ---- failure is representable -------------------------------------------------------------------

// The whole point. Config::load's ONLY nullopt path used to be "cannot read the file", so a
// truncated or corrupt app.json parsed "successfully" into all-defaults -- including
// remote_debugging_port 0, i.e. no CDP, i.e. no input at all. app.json is hot-swappable, so a
// half-written file is a normal event on that path.
void test_malformed_input_is_an_error_not_silence() {
  must_fail("");
  must_fail("   ");
  must_fail("{");
  must_fail("}");
  must_fail(R"({"url": })");
  must_fail(R"({"url" "x"})");
  must_fail(R"({url: "x"})");    // unquoted key
  must_fail(R"({'url': 'x'})");  // single quotes
  must_fail(R"({"a":1} garbage)");
  must_fail("<!DOCTYPE html>");  // an error page fetched instead of the config
  must_fail(std::string("\x01\x02\x03", 3));
  // A truncated write: the first half of a real config.
  must_fail(R"({"url":"https://www.youtube.com/tv","remote_debugging_p)");
  // Comments are not JSON. The old extractor would happily read a "commented-out" line as live
  // config, because it never tokenized at all.
  must_fail("{\"a\":1} // trailing comment");
  must_fail("{/* c */\"a\":1}");
}

void test_errors_carry_a_location() {
  ParseResult r = parse("{\n  \"a\": 1,\n  \"b\": truthy\n}");
  assert(!r.ok());
  assert(r.error.line == 3);
  assert(!r.error.message.empty());
}

void test_deep_nesting_is_bounded() {
  std::string deep;
  for (int i = 0; i < 5000; ++i) deep += "[";
  must_fail(deep);  // depth-capped, not a stack smash
}

void test_control_characters_rejected_in_strings() {
  must_fail(std::string("\"a\nb\""));  // a raw newline must be escaped
  must_fail(std::string("\"a\tb\""));
}

}  // namespace

DECKBACK_TEST_MAIN(json) {
  test_scalars();
  test_regression_bool_is_not_prefix_matched();
  test_regression_exponent_numbers();
  test_regression_string_escapes();
  test_unicode_escapes();
  test_regression_bracket_inside_a_string_does_not_end_the_array();
  test_arrays();
  test_regression_nested_key_does_not_shadow_top_level();
  test_regression_braces_inside_strings();
  test_paths();
  test_object_order_is_preserved();
  test_duplicate_keys_last_wins();
  test_leaf_paths();
  test_malformed_input_is_an_error_not_silence();
  test_errors_carry_a_location();
  test_deep_nesting_is_bounded();
  test_control_characters_rejected_in_strings();
  std::puts("json_test: ok");
  return 0;
}
