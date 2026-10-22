#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace deckback {
namespace json {

// A small JSON parser and writer (RFC 8259). No exceptions. Object key ORDER is preserved
// (`keymap` is order-significant); duplicate keys keep the LAST value.

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

  // Typed accessors. Each returns nullopt/nullptr when this value is of another type.
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
  // Used to report unknown config keys.
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

// Serialize a Value to JSON text. `indent >= 0` pretty-prints with that many spaces per level and a
// trailing newline; `indent < 0` is compact. Key order is preserved, and dump then parse yields an
// equal document.
std::string dump(const Value& v, int indent = 2);

}  // namespace json
}  // namespace deckback
