#pragma once

#include <string>
#include <string_view>
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
//   int    0x04 + 8 bytes big-endian of (x XOR sign bit)
//   float  0x05 + 8 bytes big-endian of the IEEE bits, sign-adjusted
//   text   0x06 + bytes, with 0x00 escaped as 0x00 0xFF, then 0x00 0x01
void encodeKey(const Value& v, std::string& out);
Value decodeKey(std::string_view& in);  // consumes one encoded value

// Smallest byte string greater than every string that starts with `prefix`
// (empty if there is none).
std::string prefixSuccessor(std::string prefix);

// Row payload (non-key columns): a type tag per value followed by its bytes.
void encodeRow(const Row& values, std::string& out);
// keep[i] == false skips decoding the i-th value (it comes back as null).
Row decodeRow(std::string_view in, const std::vector<bool>* keep = nullptr);
// Decodes the next row-payload value into *out, or skips it if out is null.
void decodeValue(std::string_view& in, Value* out);

}  // namespace pkql
