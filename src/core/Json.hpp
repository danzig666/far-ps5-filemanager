#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ps5 {

class JsonError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Minimal JSON DOM. Strings are stored as UTF-8. Numbers keep their source
// text so 64-bit sizes survive without a round trip through double.
class JsonValue {
public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }
  bool isObject() const { return type_ == Type::Object; }
  bool isArray() const { return type_ == Type::Array; }
  bool isString() const { return type_ == Type::String; }

  // Missing keys and out-of-range indices return a shared null value.
  const JsonValue& operator[](std::string_view key) const;
  const JsonValue& operator[](size_t index) const;
  bool has(std::string_view key) const;

  size_t size() const;
  const std::vector<JsonValue>& items() const { return array_; }
  const std::vector<std::pair<std::string, JsonValue>>& members() const { return object_; }

  std::string asString(std::string_view fallback = {}) const;
  int64_t asInt(int64_t fallback = 0) const;
  uint64_t asUInt(uint64_t fallback = 0) const;
  double asDouble(double fallback = 0) const;
  bool asBool(bool fallback = false) const;

  // Text form of a scalar, for display ("true", "123", the string itself).
  std::string toDisplay() const;

private:
  friend class JsonParser;
  Type type_ = Type::Null;
  bool bool_ = false;
  std::string text_;  // string value, or number source text
  std::vector<JsonValue> array_;
  std::vector<std::pair<std::string, JsonValue>> object_;
};

JsonValue ParseJson(std::string_view text);

}  // namespace ps5
