#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace pkql {

class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

enum class Type : uint8_t { Null = 0, Bool = 1, Int = 2, Float = 3, Text = 4 };

const char* typeName(Type t);

// A single SQL-free value. Unlike SQL there is no three-valued logic: null is
// an ordinary value that equals itself and sorts before everything else.
class Value {
 public:
  Value() = default;  // null
  static Value null() { return Value(); }
  static Value boolean(bool b) { return Value(b); }
  static Value integer(int64_t i) { return Value(i); }
  static Value real(double d) { return Value(d); }
  static Value text(std::string s) { return Value(std::move(s)); }

  Type type() const { return static_cast<Type>(v_.index()); }
  bool isNull() const { return v_.index() == 0; }
  bool isNumber() const { return type() == Type::Int || type() == Type::Float; }

  bool asBool() const { return std::get<bool>(v_); }
  int64_t asInt() const { return std::get<int64_t>(v_); }
  double asFloat() const { return std::get<double>(v_); }
  const std::string& asText() const { return std::get<std::string>(v_); }
  double asNumber() const { return type() == Type::Int ? static_cast<double>(asInt()) : asFloat(); }

  // Converts an Int to Float (other types unchanged); used when a value is
  // stored into a Float column.
  Value castTo(Type t) const;

  std::string toString() const;  // display form (text unquoted)
  std::string literal() const;    // source form (text quoted)

  size_t hash() const;
  friend bool operator==(const Value& a, const Value& b);
  friend bool operator!=(const Value& a, const Value& b) { return !(a == b); }

 private:
  explicit Value(bool b) : v_(b) {}
  explicit Value(int64_t i) : v_(i) {}
  explicit Value(double d) : v_(d) {}
  explicit Value(std::string s) : v_(std::move(s)) {}

  std::variant<std::monostate, bool, int64_t, double, std::string> v_;
};

// Total order: null < false < true < numbers (Int and Float compared
// numerically) < text (bytewise). Returns <0, 0 or >0.
int compare(const Value& a, const Value& b);

using Row = std::vector<Value>;

struct RowHash {
  size_t operator()(const Row& r) const;
};

}  // namespace pkql
