#include "json.hpp"

#include <cmath>
#include <cstdlib>

namespace deckback {
namespace json {
namespace {

// Depth cap: JSON nesting is recursive, and a hostile/corrupt file of 100k open braces would
// otherwise smash the stack before any validation ran. app.json nests two levels.
constexpr int kMaxDepth = 64;

class Parser {
 public:
  explicit Parser(std::string_view s) : s_(s) {}

  ParseResult run() {
    skip_ws();
    Value v;
    if (!parse_value(v, 0)) return fail_result();
    skip_ws();
    if (p_ != s_.size()) return err("trailing content after the top-level value"), fail_result();
    ParseResult r;
    r.value = std::move(v);
    return r;
  }

 private:
  ParseResult fail_result() {
    ParseResult r;
    r.error = err_;
    return r;
  }

  bool err(std::string msg) {
    if (err_.message.empty()) {  // keep the FIRST error: it is the one nearest the real mistake
      err_.message = std::move(msg);
      err_.offset = p_;
      err_.line = 1;
      for (size_t i = 0; i < p_ && i < s_.size(); ++i)
        if (s_[i] == '\n') ++err_.line;
    }
    return false;
  }

  bool eof() const { return p_ >= s_.size(); }
  char peek() const { return p_ < s_.size() ? s_[p_] : '\0'; }

  void skip_ws() {
    while (p_ < s_.size()) {
      const char c = s_[p_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        ++p_;
      else
        break;
    }
  }

  bool lit(std::string_view word) {
    if (s_.compare(p_, word.size(), word) != 0) return false;
    p_ += word.size();
    return true;
  }

  bool parse_value(Value& out, int depth) {
    if (depth > kMaxDepth) return err("nesting too deep");
    if (eof()) return err("unexpected end of input");
    switch (peek()) {
      case '{':
        return parse_object(out, depth);
      case '[':
        return parse_array(out, depth);
      case '"': {
        std::string s;
        if (!parse_string(s)) return false;
        out = Value(std::move(s));
        return true;
      }
      case 't':
        if (!lit("true")) return err("invalid literal");
        out = Value(true);
        return true;
      case 'f':
        if (!lit("false")) return err("invalid literal");
        out = Value(false);
        return true;
      case 'n':
        if (!lit("null")) return err("invalid literal");
        out = Value();
        return true;
      default:
        return parse_number(out);
    }
  }

  bool parse_object(Value& out, int depth) {
    ++p_;  // '{'
    std::vector<Member> members;
    skip_ws();
    if (peek() == '}') {
      ++p_;
      out = Value::object(std::move(members));
      return true;
    }
    for (;;) {
      skip_ws();
      if (peek() != '"') return err("expected a '\"'-quoted object key");
      std::string key;
      if (!parse_string(key)) return false;
      skip_ws();
      if (peek() != ':') return err("expected ':' after an object key");
      ++p_;
      skip_ws();
      Value v;
      if (!parse_value(v, depth + 1)) return false;
      // Last duplicate wins, as every mainstream parser does.
      bool replaced = false;
      for (Member& m : members)
        if (m.first == key) {
          m.second = std::move(v);
          replaced = true;
          break;
        }
      if (!replaced) members.emplace_back(std::move(key), std::move(v));
      skip_ws();
      if (peek() == ',') {
        ++p_;
        continue;
      }
      if (peek() == '}') {
        ++p_;
        out = Value::object(std::move(members));
        return true;
      }
      return err("expected ',' or '}' in an object");
    }
  }

  bool parse_array(Value& out, int depth) {
    ++p_;  // '['
    std::vector<Value> items;
    skip_ws();
    if (peek() == ']') {
      ++p_;
      out = Value::array(std::move(items));
      return true;
    }
    for (;;) {
      skip_ws();
      Value v;
      if (!parse_value(v, depth + 1)) return false;
      items.push_back(std::move(v));
      skip_ws();
      if (peek() == ',') {
        ++p_;
        continue;
      }
      if (peek() == ']') {
        ++p_;
        out = Value::array(std::move(items));
        return true;
      }
      return err("expected ',' or ']' in an array");
    }
  }

  // Encode one code point as UTF-8.
  static void utf8(unsigned cp, std::string& out) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool hex4(unsigned& out) {
    if (p_ + 4 > s_.size()) return err("truncated \\u escape");
    out = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = s_[p_ + i];
      out <<= 4;
      if (c >= '0' && c <= '9')
        out |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f')
        out |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        out |= static_cast<unsigned>(c - 'A' + 10);
      else
        return err("invalid hex digit in a \\u escape");
    }
    p_ += 4;
    return true;
  }

  bool parse_string(std::string& out) {
    ++p_;  // opening quote
    out.clear();
    for (;;) {
      if (eof()) return err("unterminated string");
      const char c = s_[p_];
      if (c == '"') {
        ++p_;
        return true;
      }
      if (static_cast<unsigned char>(c) < 0x20) return err("raw control character in a string");
      if (c != '\\') {
        out.push_back(c);
        ++p_;
        continue;
      }
      ++p_;  // backslash
      if (eof()) return err("unterminated escape");
      const char e = s_[p_++];
      switch (e) {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          unsigned cp = 0;
          if (!hex4(cp)) return false;
          // Surrogate pair: a BMP-external code point arrives as \uD800-\uDBFF \uDC00-\uDFFF.
          if (cp >= 0xD800 && cp <= 0xDBFF && p_ + 1 < s_.size() && s_[p_] == '\\' &&
              s_[p_ + 1] == 'u') {
            const size_t save = p_;
            p_ += 2;
            unsigned lo = 0;
            if (!hex4(lo)) return false;
            if (lo >= 0xDC00 && lo <= 0xDFFF)
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            else
              p_ = save;  // not a low surrogate: emit the high one as-is
          }
          utf8(cp, out);
          break;
        }
        default:
          return err("invalid escape character");
      }
    }
  }

  bool parse_number(Value& out) {
    const size_t start = p_;
    if (peek() == '-') ++p_;
    if (eof() || !(peek() >= '0' && peek() <= '9')) return err("invalid number");
    // JSON forbids leading zeros: `0` and `0.5` are numbers, `01` is not. Accepting it would let a
    // zero-padded value parse as something other than it looks (and `0755` is a plausible typo in a
    // config file).
    if (s_[p_] == '0') {
      ++p_;
      if (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') return err("leading zero in a number");
    } else {
      while (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') ++p_;
    }
    if (peek() == '.') {
      ++p_;
      if (eof() || !(peek() >= '0' && peek() <= '9')) return err("invalid number: no fraction");
      while (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') ++p_;
    }
    if (peek() == 'e' || peek() == 'E') {
      ++p_;
      if (peek() == '+' || peek() == '-') ++p_;
      if (eof() || !(peek() >= '0' && peek() <= '9')) return err("invalid number: no exponent");
      while (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') ++p_;
    }
    const std::string tok(s_.substr(start, p_ - start));
    char* end = nullptr;
    const double d = std::strtod(tok.c_str(), &end);
    if (end != tok.c_str() + tok.size() || !std::isfinite(d)) return err("number out of range");
    out = Value(d);
    return true;
  }

  std::string_view s_;
  size_t p_ = 0;
  Error err_;
};

}  // namespace

Value Value::array(std::vector<Value> v) {
  Value o;
  o.type_ = Type::Array;
  o.arr_ = std::move(v);
  return o;
}

Value Value::object(std::vector<Member> m) {
  Value o;
  o.type_ = Type::Object;
  o.obj_ = std::move(m);
  return o;
}

std::optional<bool> Value::as_bool() const {
  if (type_ != Type::Bool) return std::nullopt;
  return bool_;
}

std::optional<double> Value::as_number() const {
  if (type_ != Type::Number) return std::nullopt;
  return num_;
}

const std::string* Value::as_string() const { return type_ == Type::String ? &str_ : nullptr; }
const std::vector<Value>* Value::as_array() const { return type_ == Type::Array ? &arr_ : nullptr; }
const std::vector<Member>* Value::as_object() const {
  return type_ == Type::Object ? &obj_ : nullptr;
}

const Value* Value::find(std::string_view path) const {
  const Value* cur = this;
  size_t pos = 0;
  while (pos <= path.size()) {
    const size_t dot = path.find('.', pos);
    const std::string_view seg =
        path.substr(pos, dot == std::string_view::npos ? std::string_view::npos : dot - pos);
    if (cur->type_ != Type::Object) return nullptr;
    const Value* next = nullptr;
    for (const Member& m : cur->obj_)
      if (m.first == seg) next = &m.second;
    if (!next) return nullptr;
    cur = next;
    if (dot == std::string_view::npos) return cur;
    pos = dot + 1;
  }
  return nullptr;
}

void Value::collect(std::string prefix, std::vector<std::string>& out) const {
  if (type_ != Type::Object) {
    out.push_back(std::move(prefix));
    return;
  }
  for (const Member& m : obj_)
    m.second.collect(prefix.empty() ? m.first : prefix + "." + m.first, out);
}

std::vector<std::string> Value::leaf_paths() const {
  std::vector<std::string> out;
  collect(std::string(), out);
  return out;
}

ParseResult parse(std::string_view text) { return Parser(text).run(); }

}  // namespace json
}  // namespace deckback
