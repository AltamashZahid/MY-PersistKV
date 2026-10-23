#pragma once

// Column blocks: a compressed, column-oriented copy of a table's older rows,
// read by vectorized scans (SQL Server's columnstore indexes, Larson et al.,
// SIGMOD 2011; HyPer's Data Blocks, Lang et al., SIGMOD 2016).
//
// The row store stays complete and authoritative: every row is still a
// PersistKV row, so writes, point lookups, indexes and recovery are
// unchanged. Complete runs of kBlockRows consecutive rows (in key order) are
// also encoded into one immutable block, column by column, with lightweight
// compression:
//   ints        frame of reference + bit-packing, or deltas of sorted
//               values (timestamps) + frame of reference + bit-packing
//   floats      scaled to exact integers by a power of ten when every value
//               round-trips (the idea of ALP, Afroozeh et al., SIGMOD 2024),
//               else raw 8 bytes
//   text        dictionary codes, bit-packed; plain strings if a value has
//               no code
//   nulls       a bitmap, when the column has any
// A block's metadata records its key range, row count and each int column's
// min / max (small materialized aggregates, Moerkotte, VLDB 1998), so scans
// skip blocks a filter cannot match. Any write inside a block's key range
// drops the block in the same batch; scans then read those rows from the
// row store, and a later build makes a new block.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "pkql/storage.h"
#include "pkql/vector.h"

namespace pkql {

constexpr uint32_t kBlockRows = 16384;

struct BlockColumnStats {
  bool has_range = false;  // min / max are set (Int and Bool columns with a non-null value)
  int64_t min = 0, max = 0;
  bool has_nulls = false;
};

struct BlockMeta {
  std::string first_key, last_key;  // row keys of the block's first and last rows
  uint32_t rows = 0;
  std::vector<BlockColumnStats> cols;
};

struct ColumnBlocks {
  std::map<std::string, BlockMeta> by_first;  // first row key -> block, in key order
  uint64_t covered_rows = 0;
  bool gaps = false;  // a block was dropped: the next build re-covers its range
  // The block whose key range holds row_key, or null.
  const BlockMeta* containing(const std::string& row_key) const;
};

// The blocks whose key range a statement writes into are deleted in the
// statement's batch (scans then read those rows from the row store, and a
// later build covers them again). commit() forgets them in memory once the
// batch is written.
class BlockDrops {
 public:
  explicit BlockDrops(const TableSchema& t) : t_(t) {}
  void touch(const std::string& row_key, pkv::WriteBatch& batch);
  void commit();
  bool any() const { return !dropped_.empty(); }

 private:
  const TableSchema& t_;
  std::vector<std::string> dropped_;  // first keys of the blocks to drop
};

// Tables with a single Int primary key (time-keyed event tables) get blocks.
bool blocksEligible(const TableSchema& t);

std::string blockKey(const TableSchema& t, const std::string& first_row_key);      // the encoded block
std::string blockMetaKey(const TableSchema& t, const std::string& first_row_key);  // its metadata
std::string blockMetaPrefix(uint32_t table_id);
std::string serializeBlockMeta(const BlockMeta& m);
// first_row_key comes from the metadata's key.
BlockMeta deserializeBlockMeta(std::string_view bytes, std::string first_row_key);
std::string firstRowKeyFromMetaKey(const TableSchema& t, std::string_view meta_key);

// Encodes rows (sorted by key, all of one table) into a block and fills
// *meta (key range, count, column statistics).
std::string encodeBlock(const TableSchema& t, const std::vector<std::string>& keys, const std::vector<Row>& rows,
                        BlockMeta* meta);
// Decodes the needed columns (all if needed is null) into cols, each sized to
// the block's row count; returns that count. Text columns get the table's
// dictionary, so coded values stay codes.
uint32_t decodeBlock(const TableSchema& t, std::string_view blob, const std::vector<bool>* needed,
                     std::vector<ColumnVector>& cols);

}  // namespace pkql
