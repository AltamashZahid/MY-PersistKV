#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pkql/value.h"

namespace pkql {

// Order-preserving ("memcomparable") key encoding: for values of the same
// column type, memcmp(encode(a), encode(b)) has the same sign as
// compare(a, b). A multi-column key is the concatenation of its encoded
// columns, so lexicographic byte order equals tuple order and the B+Tree's
// range scans return rows in index order.
//
//   null   0x01
//   false  0x02,  true 0x03
//   int    tag 0x10..0x21 giving sign and length, then 0-8 big-endian bytes
//          (negatives complemented), so small numbers take 1-2 bytes
//   float  0x05 + 8 bytes big-endian of the IEEE bits, sign-adjusted
//   text   0x06 + bytes, with 0x00 escaped as 0x00 0xFF, then 0x00 0x01
void encodeKey(const Value& v, std::string& out);
Value decodeKey(std::string_view& in);  // consumes one encoded value
void decodeKeyInto(std::string_view& in, Value* out);  // same, into *out (or skip if null)

// Smallest byte string greater than every string that starts with `prefix`
// (empty if there is none).
std::string prefixSuccessor(std::string prefix);

// Row payload (non-key columns): a type tag per value followed by its bytes.
void encodeRow(const Row& values, std::string& out);

// Dictionary encoding for a text column (Abadi et al., "Integrating
// Compression and Execution in Column-Oriented Database Systems", SIGMOD
// 2006): each distinct value gets a small integer code, rows store the code,
// and equality filters and grouping compare codes instead of strings. Codes
// are append-only and never reused; once a dictionary is full, new values
// are stored as plain text (so every reader must accept both forms).
struct TextDict {
  static constexpr size_t kMaxValues = 4096;  // codes fit a 2-byte varint
  struct Hash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return std::hash<std::string_view>()(s); }
  };
  std::vector<std::string> values;  // code -> value
  std::unordered_map<std::string, uint32_t, Hash, std::equal_to<>> codes;

  int32_t find(std::string_view s) const {  // -1 if absent
    auto it = codes.find(s);
    return it == codes.end() ? -1 : static_cast<int32_t>(it->second);
  }
  // The value's code, adding it if there is room; -1 when full.
  int32_t codeFor(const std::string& s) {
    int32_t c = find(s);
    if (c >= 0 || values.size() >= kMaxValues) return c;
    codes.emplace(s, static_cast<uint32_t>(values.size()));
    values.push_back(s);
    return static_cast<int32_t>(values.size() - 1);
  }
  void truncate(size_t n) {  // forgets codes >= n (a statement that did not commit)
    for (size_t i = n; i < values.size(); i++) codes.erase(values[i]);
    values.resize(std::min(n, values.size()));
  }
};

// One value of a row payload; text found in (or added to) `dict` is stored
// as its code.
void encodeValue(const Value& v, std::string& out, TextDict* dict = nullptr);
// keep[i] == false skips decoding the i-th value (it comes back as null).
Row decodeRow(std::string_view in, const std::vector<bool>* keep = nullptr);
// Decodes the next row-payload value into *out, or skips it if out is null.
void decodeValue(std::string_view& in, Value* out, const TextDict* dict = nullptr);  // one row value (skipped if out is null)

}  // namespace pkql
