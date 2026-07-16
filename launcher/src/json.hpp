#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace deckback {
namespace json {

// A real JSON parser (RFC 8259), replacing the byte-scanning extractor config.cpp used to carry.
//
// That extractor found a key with an unanchored `s.find("\"key\"")` over the raw file and read
// whatever followed the next colon. It could not represent a parse error, so a truncated or corrupt
// app.json parsed "successfully" into all-defaults -- which includes remote_debugging_port 0, i.e.
// no CDP, i.e. no navigator/gamepad/OSD at all. It ended an array at the first `]` BYTE, so the
// shipped `voice_mic_selectors` yielded one truncated selector instead of five. It dropped escape
// markers, so "a\nb" became "anb" on error_title/error_hint -- the designated R1 config-push
// surface. It prefix-matched booleans (`truthy` -> true) and ran std::stol over the rest of the
// file (`5e6` -> 5). And it ignored nesting entirely, so a nested key silently shadowed a top-level
// one of the same name.
//
// app.json is HOT-SWAPPABLE by design (doc §6): it is the surface for fixing production without a
// rebuild, and a partially-written file is a normal event on that path, not an exotic one. It has
// to parse correctly or say so.
//
// Deliberately small: no allocation-heavy variant, no exceptions on the hot path, ~250 lines, and
// object key ORDER is preserved because `keymap` is order-significant (startup logs and the
// first-run card list bindings the way the user wrote them). Duplicate keys keep the LAST value,
// matching every mainstream parser (the old extractor kept the first).

enum class Type { Null, Bool, Number, String, Array, Object };

class Value;
using Member = std::pair<std::string, Value>;

class Value {
 public:
  Value() = default;
  explicit Value(bool b) : type_(Type::Bool), bool_(b) {}
  explicit Value(double n) : type_(Type::Number), num_(n) {}
  explicit Value(std::string s) : type_(Type::String), str_(std::move(s)) {}

  static Value array(std::vector<Value> v);
  static Value object(std::vector<Member> m);

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  // Typed accessors. Each returns nullopt when this value is of another type, so a caller can
  // distinguish "absent" from "present but the wrong type" -- the distinction the old reader
  // collapsed into "keep the default, say nothing".
  std::optional<bool> as_bool() const;
  std::optional<double> as_number() const;
  const std::string* as_string() const;
  const std::vector<Value>* as_array() const;
  const std::vector<Member>* as_object() const;

  // Member lookup on an object. `path` is dot-separated ("power.devtools_poll_ms"); a single
  // segment is a plain top-level key. Returns nullptr when any segment is missing or a non-object
  // is traversed. Last duplicate wins.
  const Value* find(std::string_view path) const;

  // Every leaf path in this document, dot-joined, in document order. Array values count as leaves.
  // Used to report keys the launcher does not understand -- silence there means a typo in an
  // emergency config push does nothing, quietly.
  std::vector<std::string> leaf_paths() const;

 private:
  void collect(std::string prefix, std::vector<std::string>& out) const;

  Type type_ = Type::Null;
  bool bool_ = false;
  double num_ = 0;
  std::string str_;
  std::vector<Value> arr_;
  std::vector<Member> obj_;
};

struct Error {
  size_t offset = 0;  // byte offset into the input
  int line = 1;
  std::string message;
};

struct ParseResult {
  std::optional<Value> value;  // set on success
  Error error;                 // meaningful only when `value` is unset
  bool ok() const { return value.has_value(); }
};

// Parse a complete JSON document. Trailing whitespace is fine; trailing garbage is an error.
// Never throws.
ParseResult parse(std::string_view text);

}  // namespace json
}  // namespace deckback
