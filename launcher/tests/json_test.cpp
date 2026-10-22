// The JSON parser used for app.json and CDP replies.
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
  assert(!r.error.message.empty());
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

// Literals are matched whole, not by prefix.
void test_regression_bool_is_not_prefix_matched() {
  must_fail("truthy");
  must_fail("{\"a\": truthy}");
  must_fail("falsey");
  // A quoted "true" is a string, not a bool.
  Value v = must_parse("{\"a\": \"true\"}");
  assert(v.find("a")->as_bool() == std::nullopt);
  assert(*v.find("a")->as_string() == "true");
}

// Exponents parse in full; malformed numbers are errors.
void test_regression_exponent_numbers() {
  assert(must_parse("5e6").as_number() == 5000000.0);
  assert(must_parse("1.5e3").as_number() == 1500.0);
  assert(must_parse("-2E2").as_number() == -200.0);
  must_fail("5e");
  must_fail("01");  // JSON forbids leading zeros
  must_fail(".5");
  must_fail("1.");
  must_fail("+1");
}

// Every JSON string escape decodes.
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

// A `]` or `[` inside a string does not end or open an array.
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

// A nested key does not shadow a top-level key of the same name.
void test_regression_nested_key_does_not_shadow_top_level() {
  Value v = must_parse(R"({"keymap":{"url":"https://SHADOWED/"},"url":"https://real/"})");
  assert(*v.find("url")->as_string() == "https://real/");
  assert(*v.find("keymap.url")->as_string() == "https://SHADOWED/");
}

// Braces inside strings do not affect object structure.
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

// Object key order is preserved (keymap order drives the controls card).
void test_object_order_is_preserved() {
  Value v = must_parse(R"({"z":1,"a":2,"m":3})");
  const auto* o = v.as_object();
  assert(o->size() == 3);
  assert((*o)[0].first == "z" && (*o)[1].first == "a" && (*o)[2].first == "m");
}

// Duplicate keys: the last one wins, as in mainstream parsers.
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

// Truncated, corrupt, or non-JSON input is a parse error.
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
  // Comments are not JSON.
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

// dump() escapes every control byte, so parse() round-trips it.
void test_dump_escapes_control_characters() {
  const std::string raw("q\"\\\b\f\n\r\t\x01\x1f\0end", 14);
  const std::string text = dump(Value(raw), -1);
  assert(text.find('\x01') == std::string::npos && text.find('\0') == std::string::npos);
  assert(*must_parse(text).as_string() == raw);
}

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
  test_dump_escapes_control_characters();
  std::puts("json_test: ok");
  return 0;
}
