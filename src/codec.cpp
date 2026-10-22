#include "pkql/codec.h"

#include <cstring>

namespace pkql {

namespace {

// Tags. Integers in keys use a range of tags that also encodes their sign
// and byte length (see encodeKey); 0x04 is the integer tag in row payloads.
constexpr uint8_t kNull = 0x01, kFalse = 0x02, kTrue = 0x03, kInt = 0x04, kFloat = 0x05, kText = 0x06;
constexpr uint8_t kIntNegBase = 0x18;  // negative ints: 0x10..0x18
constexpr uint8_t kIntPosBase = 0x19;  // non-negative ints: 0x19..0x21

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

int bytesNeeded(uint64_t x) {
  int n = 0;
  while (x) {
    n++;
    x >>= 8;
  }
  return n;
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

void putVarint(uint64_t x, std::string& out) {
  while (x >= 0x80) {
    out += static_cast<char>((x & 0x7F) | 0x80);
    x >>= 7;
  }
  out += static_cast<char>(x);
}

uint64_t getVarint(std::string_view& in) {
  uint64_t x = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (in.empty()) throw Error("corrupt row: truncated varint");
    uint8_t b = static_cast<uint8_t>(in[0]);
    in.remove_prefix(1);
    x |= static_cast<uint64_t>(b & 0x7F) << shift;
    if (!(b & 0x80)) return x;
  }
  throw Error("corrupt row: varint too long");
}

uint64_t zigzag(int64_t v) { return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63); }
int64_t unzigzag(uint64_t u) { return static_cast<int64_t>(u >> 1) ^ -static_cast<int64_t>(u & 1); }

}  // namespace

void encodeKey(const Value& v, std::string& out) {
  switch (v.type()) {
    case Type::Null: out += static_cast<char>(kNull); break;
    case Type::Bool: out += static_cast<char>(v.asBool() ? kTrue : kFalse); break;
    case Type::Int: {
      // Variable length but still order-preserving: the tag says the sign
      // and how many bytes follow, so small numbers take 1-2 bytes. More
      // bytes means a larger magnitude, so tags grow with positive length
      // and shrink with negative length; negative payloads are complemented
      // so that larger magnitudes sort lower.
      int64_t x = v.asInt();
      if (x >= 0) {
        uint64_t u = static_cast<uint64_t>(x);
        int n = bytesNeeded(u);
        out += static_cast<char>(kIntPosBase + n);
        for (int i = n - 1; i >= 0; i--) out += static_cast<char>((u >> (8 * i)) & 0xFF);
      } else {
        uint64_t w = ~static_cast<uint64_t>(x);  // -x - 1
        int n = bytesNeeded(w);
        out += static_cast<char>(kIntNegBase - n);
        for (int i = n - 1; i >= 0; i--) out += static_cast<char>(0xFF - ((w >> (8 * i)) & 0xFF));
      }
      break;
    }
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
  if (tag >= kIntNegBase - 8 && tag <= kIntPosBase + 8) {
    bool negative = tag <= kIntNegBase;
    int n = negative ? kIntNegBase - tag : tag - kIntPosBase;
    if (in.size() < static_cast<size_t>(n)) throw Error("corrupt key: truncated integer");
    uint64_t u = 0;
    for (int i = 0; i < n; i++) {
      uint8_t b = static_cast<uint8_t>(in[i]);
      u = (u << 8) | (negative ? 0xFF - b : b);
    }
    in.remove_prefix(n);
    return Value::integer(negative ? static_cast<int64_t>(~u) : static_cast<int64_t>(u));
  }
  switch (tag) {
    case kNull: return Value::null();
    case kFalse: return Value::boolean(false);
    case kTrue: return Value::boolean(true);
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

// Row payloads: integers as zigzag varints (small numbers take 1-2 bytes),
// text as a varint length plus bytes, floats as 8 raw bytes.
void encodeRow(const Row& values, std::string& out) {
  for (const Value& v : values) {
    switch (v.type()) {
      case Type::Null: out += static_cast<char>(kNull); break;
      case Type::Bool: out += static_cast<char>(v.asBool() ? kTrue : kFalse); break;
      case Type::Int:
        out += static_cast<char>(kInt);
        putVarint(zigzag(v.asInt()), out);
        break;
      case Type::Float: {
        out += static_cast<char>(kFloat);
        double d = v.asFloat();
        out.append(reinterpret_cast<const char*>(&d), 8);
        break;
      }
      case Type::Text:
        out += static_cast<char>(kText);
        putVarint(v.asText().size(), out);
        out += v.asText();
        break;
    }
  }
}

void decodeValue(std::string_view& in, Value* out) {
  if (in.empty()) throw Error("corrupt row");
  uint8_t tag = static_cast<uint8_t>(in[0]);
  in.remove_prefix(1);
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
      int64_t x = unzigzag(getVarint(in));
      if (out) *out = Value::integer(x);
      return;
    }
    case kFloat: {
      if (in.size() < 8) throw Error("corrupt row");
      if (out) {
        double d;
        std::memcpy(&d, in.data(), 8);
        *out = Value::real(d);
      }
      in.remove_prefix(8);
      return;
    }
    case kText: {
      uint64_t n = getVarint(in);
      if (in.size() < n) throw Error("corrupt row");
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
    bool want = !keep || row.size() >= keep->size() || (*keep)[row.size()];
    row.emplace_back();
    decodeValue(in, want ? &row.back() : nullptr);
  }
  return row;
}

}  // namespace pkql
