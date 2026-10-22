#include "pkql/value.h"

#include <cmath>
#include <cstdio>
#include <functional>

namespace pkql {

const char* typeName(Type t) {
  switch (t) {
    case Type::Null: return "null";
    case Type::Bool: return "bool";
    case Type::Int: return "int";
    case Type::Float: return "float";
    case Type::Text: return "text";
  }
  return "?";
}

Value Value::castTo(Type t) const {
  if (t == Type::Float && type() == Type::Int) return Value::real(static_cast<double>(asInt()));
  return *this;
}

std::string Value::toString() const {
  switch (type()) {
    case Type::Null: return "null";
    case Type::Bool: return asBool() ? "true" : "false";
    case Type::Int: return std::to_string(asInt());
    case Type::Float: {
      char buf[64];
      std::snprintf(buf, sizeof buf, "%.15g", asFloat());
      std::string s = buf;
      if (s.find_first_of(".eEn") == std::string::npos) s += ".0";  // keep it visibly a float
      return s;
    }
    case Type::Text: return asText();
  }
  return "";
}

std::string Value::literal() const {
  if (type() != Type::Text) return toString();
  std::string out = "\"";
  for (char c : asText()) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

namespace {

int rank(Type t) {
  switch (t) {
    case Type::Null: return 0;
    case Type::Bool: return 1;
    case Type::Int:
    case Type::Float: return 2;
    case Type::Text: return 3;
  }
  return 4;
}

}  // namespace

int compare(const Value& a, const Value& b) {
  int ra = rank(a.type()), rb = rank(b.type());
  if (ra != rb) return ra < rb ? -1 : 1;
  switch (a.type()) {
    case Type::Null: return 0;
    case Type::Bool: return static_cast<int>(a.asBool()) - static_cast<int>(b.asBool());
    case Type::Int:
      if (b.type() == Type::Int) return a.asInt() < b.asInt() ? -1 : (a.asInt() > b.asInt() ? 1 : 0);
      [[fallthrough]];
    case Type::Float: {
      double x = a.asNumber(), y = b.asNumber();
      return x < y ? -1 : (x > y ? 1 : 0);
    }
    case Type::Text: {
      int c = a.asText().compare(b.asText());
      return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
  }
  return 0;
}

bool operator==(const Value& a, const Value& b) { return compare(a, b) == 0; }

size_t Value::hash() const {
  switch (type()) {
    case Type::Null: return 0x9e3779b9u;
    case Type::Bool: return asBool() ? 1u : 2u;
    case Type::Int:
    case Type::Float: {
      // Equal numbers must hash equally even across Int and Float.
      double d = asNumber();
      if (d == std::floor(d) && std::fabs(d) < 9e15) return std::hash<int64_t>()(static_cast<int64_t>(d));
      return std::hash<double>()(d);
    }
    case Type::Text: return std::hash<std::string>()(asText());
  }
  return 0;
}

size_t RowHash::operator()(const Row& r) const {
  size_t h = 1469598103934665603ull;
  for (const Value& v : r) h = (h ^ v.hash()) * 1099511628211ull;
  return h;
}

}  // namespace pkql
