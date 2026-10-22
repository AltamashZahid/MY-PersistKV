#include "pkql/codec.h"

#include <cstring>

namespace pkql {

namespace {

constexpr uint8_t kNull = 0x01, kFalse = 0x02, kTrue = 0x03, kInt = 0x04, kFloat = 0x05, kText = 0x06;

void putBigEndian(uint64_t x, std::string& out) {
  for (int shift = 56; shift >= 0; shift -= 8) out += static_cast<char>((x >> shift) & 0xFF);
}

uint64_t getBigEndian(std::string_view& in) {
  if (in.size() < 8) throw Error("corrupt key: truncated number");
  uint64_t x = 0;
  for (int i = 0; i < 8; i++) x = (x << 8) | static_cast<uint8_t>(in[i]);
  in.remove_prefix(8);
  return x;
}

uint64_t floatToOrdered(double d) {
  if (d == 0) d = 0;  // -0.0 and 0.0 compare equal, so they must encode equally
  uint64_t bits;
  std::memcpy(&bits, &d, 8);
  // Positive floats: set the sign bit so they sort above negatives.
  // Negative floats: flip all bits so larger magnitudes sort lower.
  return (bits & (1ull << 63)) ? ~bits : bits | (1ull << 63);
}

double orderedToFloat(uint64_t x) {
  uint64_t bits = (x & (1ull << 63)) ? x & ~(1ull << 63) : ~x;
  double d;
  std::memcpy(&d, &bits, 8);
  return d;
}

}  // namespace

void encodeKey(const Value& v, std::string& out) {
  switch (v.type()) {
    case Type::Null: out += static_cast<char>(kNull); break;
    case Type::Bool: out += static_cast<char>(v.asBool() ? kTrue : kFalse); break;
    case Type::Int:
      out += static_cast<char>(kInt);
      putBigEndian(static_cast<uint64_t>(v.asInt()) ^ (1ull << 63), out);
      break;
    case Type::Float:
      out += static_cast<char>(kFloat);
      putBigEndian(floatToOrdered(v.asFloat()), out);
      break;
    case Type::Text:
      out += static_cast<char>(kText);
      for (char c : v.asText()) {
        out += c;
        if (c == '\0') out += '\xFF';
      }
      out += '\0';
      out += '\x01';
      break;
  }
}

Value decodeKey(std::string_view& in) {
  if (in.empty()) throw Error("corrupt key: missing value");
  uint8_t tag = static_cast<uint8_t>(in[0]);
  in.remove_prefix(1);
  switch (tag) {
    case kNull: return Value::null();
    case kFalse: return Value::boolean(false);
    case kTrue: return Value::boolean(true);
    case kInt: return Value::integer(static_cast<int64_t>(getBigEndian(in) ^ (1ull << 63)));
    case kFloat: return Value::real(orderedToFloat(getBigEndian(in)));
    case kText: {
      std::string s;
      while (true) {
        if (in.empty()) throw Error("corrupt key: unterminated text");
        char c = in[0];
        if (c != '\0') {
          s += c;
          in.remove_prefix(1);
          continue;
        }
        if (in.size() < 2) throw Error("corrupt key: unterminated text");
        if (in[1] == '\x01') {
          in.remove_prefix(2);
          return Value::text(std::move(s));
        }
        s += '\0';  // escaped zero byte (0x00 0xFF)
        in.remove_prefix(2);
      }
    }
  }
  throw Error("corrupt key: unknown type tag");
}

std::string prefixSuccessor(std::string prefix) {
  while (!prefix.empty()) {
    unsigned char last = static_cast<unsigned char>(prefix.back());
    if (last != 0xFF) {
      prefix.back() = static_cast<char>(last + 1);
      return prefix;
    }
    prefix.pop_back();
  }
  return prefix;
}

void encodeRow(const Row& values, std::string& out) {
  for (const Value& v : values) {
    switch (v.type()) {
      case Type::Null: out += static_cast<char>(kNull); break;
      case Type::Bool: out += static_cast<char>(v.asBool() ? kTrue : kFalse); break;
      case Type::Int: {
        out += static_cast<char>(kInt);
        int64_t x = v.asInt();
        out.append(reinterpret_cast<const char*>(&x), 8);
        break;
      }
      case Type::Float: {
        out += static_cast<char>(kFloat);
        double d = v.asFloat();
        out.append(reinterpret_cast<const char*>(&d), 8);
        break;
      }
      case Type::Text: {
        out += static_cast<char>(kText);
        uint32_t n = static_cast<uint32_t>(v.asText().size());
        out.append(reinterpret_cast<const char*>(&n), 4);
        out += v.asText();
        break;
      }
    }
  }
}

void decodeValue(std::string_view& in, Value* out) {
  if (in.empty()) throw Error("corrupt row");
  uint8_t tag = static_cast<uint8_t>(in[0]);
  in.remove_prefix(1);
  auto need = [&](size_t n) {
    if (in.size() < n) throw Error("corrupt row");
  };
  switch (tag) {
    case kNull:
      if (out) *out = Value::null();
      return;
    case kFalse:
      if (out) *out = Value::boolean(false);
      return;
    case kTrue:
      if (out) *out = Value::boolean(true);
      return;
    case kInt: {
      need(8);
      if (out) {
        int64_t x;
        std::memcpy(&x, in.data(), 8);
        *out = Value::integer(x);
      }
      in.remove_prefix(8);
      return;
    }
    case kFloat: {
      need(8);
      if (out) {
        double d;
        std::memcpy(&d, in.data(), 8);
        *out = Value::real(d);
      }
      in.remove_prefix(8);
      return;
    }
    case kText: {
      need(4);
      uint32_t n;
      std::memcpy(&n, in.data(), 4);
      in.remove_prefix(4);
      need(n);
      if (out) *out = Value::text(std::string(in.substr(0, n)));
      in.remove_prefix(n);
      return;
    }
    default:
      throw Error("corrupt row: unknown type tag");
  }
}

Row decodeRow(std::string_view in, const std::vector<bool>* keep) {
  Row row;
  while (!in.empty()) {
    uint8_t tag = static_cast<uint8_t>(in[0]);
    in.remove_prefix(1);
    if (keep && row.size() < keep->size() && !(*keep)[row.size()]) {
      // Skip the bytes without building a value.
      size_t n = 0;
      if (tag == kInt || tag == kFloat) n = 8;
      else if (tag == kText) {
        if (in.size() < 4) throw Error("corrupt row");
        uint32_t len;
        std::memcpy(&len, in.data(), 4);
        n = 4 + len;
      } else if (tag != kNull && tag != kFalse && tag != kTrue) {
        throw Error("corrupt row: unknown type tag");
      }
      if (in.size() < n) throw Error("corrupt row");
      in.remove_prefix(n);
      row.emplace_back();
      continue;
    }
    auto need = [&](size_t n) {
      if (in.size() < n) throw Error("corrupt row");
    };
    switch (tag) {
      case kNull: row.push_back(Value::null()); break;
      case kFalse: row.push_back(Value::boolean(false)); break;
      case kTrue: row.push_back(Value::boolean(true)); break;
      case kInt: {
        need(8);
        int64_t x;
        std::memcpy(&x, in.data(), 8);
        in.remove_prefix(8);
        row.push_back(Value::integer(x));
        break;
      }
      case kFloat: {
        need(8);
        double d;
        std::memcpy(&d, in.data(), 8);
        in.remove_prefix(8);
        row.push_back(Value::real(d));
        break;
      }
      case kText: {
        need(4);
        uint32_t n;
        std::memcpy(&n, in.data(), 4);
        in.remove_prefix(4);
        need(n);
        row.push_back(Value::text(std::string(in.substr(0, n))));
        in.remove_prefix(n);
        break;
      }
      default: throw Error("corrupt row: unknown type tag");
    }
  }
  return row;
}

}  // namespace pkql
