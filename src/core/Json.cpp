#include "Json.hpp"

#include <cstdlib>
#include <cerrno>

#include "Text.hpp"

namespace ps5 {

namespace {
const JsonValue& NullValue() {
  static const JsonValue value;
  return value;
}
}  // namespace

const JsonValue& JsonValue::operator[](std::string_view key) const {
  if (type_ == Type::Object) {
    for (const auto& member : object_) {
      if (member.first == key) return member.second;
    }
  }
  return NullValue();
}

const JsonValue& JsonValue::operator[](size_t index) const {
  if (type_ == Type::Array && index < array_.size()) return array_[index];
  return NullValue();
}

bool JsonValue::has(std::string_view key) const {
  if (type_ != Type::Object) return false;
  for (const auto& member : object_) {
    if (member.first == key) return true;
  }
  return false;
}

size_t JsonValue::size() const {
  if (type_ == Type::Array) return array_.size();
  if (type_ == Type::Object) return object_.size();
  return 0;
}

std::string JsonValue::asString(std::string_view fallback) const {
  if (type_ == Type::String || type_ == Type::Number) return text_;
  return std::string(fallback);
}

int64_t JsonValue::asInt(int64_t fallback) const {
  if (type_ == Type::Number) {
    errno = 0;
    char* end = nullptr;
    long long value = std::strtoll(text_.c_str(), &end, 10);
    if (errno == 0 && end && *end == 0) return value;
    return static_cast<int64_t>(std::strtod(text_.c_str(), nullptr));
  }
  if (type_ == Type::Bool) return bool_ ? 1 : 0;
  return fallback;
}

uint64_t JsonValue::asUInt(uint64_t fallback) const {
  if (type_ == Type::Number) {
    if (!text_.empty() && text_[0] == '-') return 0;
    errno = 0;
    char* end = nullptr;
    unsigned long long value = std::strtoull(text_.c_str(), &end, 10);
    if (errno == 0 && end && *end == 0) return value;
    return static_cast<uint64_t>(std::strtod(text_.c_str(), nullptr));
  }
  if (type_ == Type::Bool) return bool_ ? 1 : 0;
  return fallback;
}

double JsonValue::asDouble(double fallback) const {
  if (type_ == Type::Number) return std::strtod(text_.c_str(), nullptr);
  return fallback;
}

bool JsonValue::asBool(bool fallback) const {
  if (type_ == Type::Bool) return bool_;
  if (type_ == Type::Number) return asDouble() != 0;
  return fallback;
}

std::string JsonValue::toDisplay() const {
  switch (type_) {
    case Type::Null: return "null";
    case Type::Bool: return bool_ ? "true" : "false";
    case Type::Number:
    case Type::String: return text_;
    case Type::Array: return "[...]";
    case Type::Object: return "{...}";
  }
  return {};
}

class JsonParser {
public:
  explicit JsonParser(std::string_view text) : text_(text) {}

  JsonValue parseDocument() {
    JsonValue value = parseValue(0);
    skipSpace();
    if (pos_ != text_.size()) fail("trailing characters");
    return value;
  }

private:
  static constexpr int kMaxDepth = 64;

  [[noreturn]] void fail(const char* what) {
    throw JsonError(std::string("JSON parse error: ") + what + " at offset " +
                    std::to_string(pos_));
  }

  void skipSpace() {
    while (pos_ < text_.size()) {
      char c = text_[pos_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
      ++pos_;
    }
  }

  bool consume(std::string_view word) {
    if (text_.substr(pos_, word.size()) == word) {
      pos_ += word.size();
      return true;
    }
    return false;
  }

  JsonValue parseValue(int depth) {
    if (depth > kMaxDepth) fail("nesting too deep");
    skipSpace();
    if (pos_ >= text_.size()) fail("unexpected end");
    JsonValue value;
    char c = text_[pos_];
    if (c == '{') {
      value.type_ = JsonValue::Type::Object;
      ++pos_;
      skipSpace();
      if (pos_ < text_.size() && text_[pos_] == '}') {
        ++pos_;
        return value;
      }
      for (;;) {
        skipSpace();
        if (pos_ >= text_.size() || text_[pos_] != '"') fail("expected key");
        std::string key = parseString();
        skipSpace();
        if (pos_ >= text_.size() || text_[pos_] != ':') fail("expected ':'");
        ++pos_;
        JsonValue member = parseValue(depth + 1);
        value.object_.emplace_back(std::move(key), std::move(member));
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == ',') {
          ++pos_;
          continue;
        }
        if (pos_ < text_.size() && text_[pos_] == '}') {
          ++pos_;
          return value;
        }
        fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      value.type_ = JsonValue::Type::Array;
      ++pos_;
      skipSpace();
      if (pos_ < text_.size() && text_[pos_] == ']') {
        ++pos_;
        return value;
      }
      for (;;) {
        value.array_.push_back(parseValue(depth + 1));
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == ',') {
          ++pos_;
          continue;
        }
        if (pos_ < text_.size() && text_[pos_] == ']') {
          ++pos_;
          return value;
        }
        fail("expected ',' or ']'");
      }
    }
    if (c == '"') {
      value.type_ = JsonValue::Type::String;
      value.text_ = parseString();
      return value;
    }
    if (consume("true")) {
      value.type_ = JsonValue::Type::Bool;
      value.bool_ = true;
      return value;
    }
    if (consume("false")) {
      value.type_ = JsonValue::Type::Bool;
      return value;
    }
    if (consume("null")) return value;
    if (c == '-' || (c >= '0' && c <= '9')) {
      size_t start = pos_;
      if (text_[pos_] == '-') ++pos_;
      while (pos_ < text_.size()) {
        char d = text_[pos_];
        if ((d >= '0' && d <= '9') || d == '.' || d == 'e' || d == 'E' || d == '+' || d == '-') {
          ++pos_;
        } else {
          break;
        }
      }
      value.type_ = JsonValue::Type::Number;
      value.text_ = std::string(text_.substr(start, pos_ - start));
      return value;
    }
    fail("unexpected character");
  }

  unsigned parseHex4() {
    if (pos_ + 4 > text_.size()) fail("short \\u escape");
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) {
      char h = text_[pos_++];
      value <<= 4;
      if (h >= '0' && h <= '9') value |= unsigned(h - '0');
      else if (h >= 'a' && h <= 'f') value |= unsigned(h - 'a' + 10);
      else if (h >= 'A' && h <= 'F') value |= unsigned(h - 'A' + 10);
      else fail("bad \\u escape");
    }
    return value;
  }

  std::string parseString() {
    ++pos_;  // opening quote
    std::string out;
    while (pos_ < text_.size()) {
      char c = text_[pos_++];
      if (c == '"') return out;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) break;
      char e = text_[pos_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          char32_t cp = parseHex4();
          if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= text_.size() &&
              text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
            size_t save = pos_;
            pos_ += 2;
            char32_t low = parseHex4();
            if (low >= 0xDC00 && low <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else {
              pos_ = save;
            }
          }
          AppendUtf8(out, cp);
          break;
        }
        default: fail("bad escape");
      }
    }
    fail("unterminated string");
  }

  std::string_view text_;
  size_t pos_ = 0;
};

JsonValue ParseJson(std::string_view text) {
  return JsonParser(text).parseDocument();
}

}  // namespace ps5
