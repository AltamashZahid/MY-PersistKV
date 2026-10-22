#pragma once

// Vectorized execution: rows are decoded in batches straight into typed
// column arrays, filters run as tight loops over those arrays producing a
// selection vector, and aggregates / top-N consume the arrays directly.
// Used for "aggregate or top-N directly over a table scan"; every other plan
// shape runs row at a time (exec.cpp). Same results either way.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pkql/exec.h"
#include "pkql/value.h"

namespace pkql {

constexpr size_t kBatchSize = 1024;

// One column of a batch. Int and Bool values live in `ints`, Float in
// `floats`, Text in `texts`; `nulls[i]` marks a null in row i.
struct ColumnVector {
  Type type = Type::Null;
  std::vector<int64_t> ints;
  std::vector<double> floats;
  std::vector<std::string> texts;
  std::vector<uint8_t> nulls;

  void reserve(Type t, size_t n);
  Value get(size_t i) const;  // the value of row i as a Value
};

// Typed decoders (codec.cpp): decode the next encoded value into row `i` of
// `col`, or skip over it when `col` is null.
void decodeKeyToColumn(std::string_view& in, ColumnVector* col, size_t i);
void decodeValueToColumn(std::string_view& in, ColumnVector* col, size_t i);

// Marks plan nodes that the vectorized executor can run (optimizer pass).
void markVectorized(PNode& plan);

// Builds the operator for a node marked vectorized.
std::unique_ptr<Operator> buildVectorizedOperator(PNode& plan, ExecContext& ctx);

}  // namespace pkql
