#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "persistkv/db.h"
#include "pkql/value.h"

namespace pkql {

// ---------------------------------------------------------------------------
// Schema and statistics
// ---------------------------------------------------------------------------

struct Column {
  std::string name;
  Type type = Type::Int;
  bool nullable = false;
};

struct IndexSchema {
  uint32_t id = 0;
  std::string name;
  std::vector<int> cols;  // column positions in the table, in index order
};

// Per-column statistics gathered by `analyze`.
struct ColumnStats {
  uint64_t ndv = 0;    // number of distinct values
  uint64_t nulls = 0;
  Value min, max;
  // Equi-depth histogram: bounds[0] = min, bounds.back() = max, and each of
  // the bounds.size() - 1 buckets holds roughly the same number of rows.
  std::vector<Value> bounds;
};

struct TableStats {
  uint64_t rows = 0;      // maintained exactly by every insert and delete
  bool analyzed = false;  // column statistics below are valid
  std::vector<ColumnStats> cols;
};

struct TableSchema {
  uint32_t id = 0;
  std::string name;
  std::vector<Column> cols;
  std::vector<int> pk;  // primary key column positions, in key order
  std::vector<IndexSchema> indexes;
  TableStats stats;

  int colIndex(const std::string& col) const;  // -1 if absent
  bool isPkColumn(int col) const;
};

// ---------------------------------------------------------------------------
// Key layout in PersistKV
//
//   r <table id u32 BE> <encoded pk columns>          -> encoded non-key columns
//   x <index id u32 BE> <encoded index cols> <pk>     -> (empty)
//   m t <table name>                                  -> table schema
//   m s <table id u32 BE>                             -> table statistics
//   m n                                               -> next id
//
// Because the encoding is order-preserving, a table's rows are stored in
// primary-key order and an index's entries in index-column order.
// ---------------------------------------------------------------------------

std::string tablePrefix(uint32_t table_id);
std::string indexPrefix(uint32_t index_id);
std::string encodeKeyColumns(const Row& row, const std::vector<int>& cols);
std::string rowKey(const TableSchema& t, const Row& row);
std::string rowPayload(const TableSchema& t, const Row& row);
std::string indexKey(const TableSchema& t, const IndexSchema& ix, const Row& row);
// `needed` (one flag per table column) lets the caller skip decoding columns
// the query never reads; they come back as null.
Row decodeStoredRow(const TableSchema& t, std::string_view key, std::string_view payload,
                    const std::vector<bool>* needed = nullptr);
// The primary key tail of an index entry, turned back into a row key.
std::string rowKeyFromIndexEntry(const TableSchema& t, const IndexSchema& ix, std::string_view entry);

// ---------------------------------------------------------------------------
// Catalog: the table schemas, persisted in the same store as the data.
// ---------------------------------------------------------------------------

class Catalog {
 public:
  explicit Catalog(pkv::DB& db) : db_(db) {}

  void load();
  TableSchema* find(const std::string& name);
  const std::map<std::string, std::unique_ptr<TableSchema>>& tables() const { return tables_; }

  uint32_t allocateId(pkv::WriteBatch& batch);
  void stageTable(const TableSchema& t, pkv::WriteBatch& batch);  // schema + stats
  void stageStats(const TableSchema& t, pkv::WriteBatch& batch);
  void stageDrop(const TableSchema& t, pkv::WriteBatch& batch);

  // Apply in-memory changes after the batch has committed.
  void install(std::unique_ptr<TableSchema> t);
  void erase(const std::string& name);

 private:
  pkv::DB& db_;
  std::map<std::string, std::unique_ptr<TableSchema>> tables_;
  uint32_t next_id_ = 1;
};

// ---------------------------------------------------------------------------
// Cursor: pull-style iteration over a key range [lo, hi). PersistKV's scan
// is callback-based and holds the tree lock while it runs, so the cursor
// fetches a chunk of entries per scan and resumes after the last key.
// ---------------------------------------------------------------------------

class Cursor {
 public:
  Cursor(pkv::DB& db, std::string lo, std::string hi_exclusive)
      : db_(db), next_(std::move(lo)), hi_(std::move(hi_exclusive)) {}

  bool next(std::string* key, std::string* value);

 private:
  void fill();

  pkv::DB& db_;
  std::string next_;  // first key of the next chunk
  std::string hi_;    // exclusive; empty = unbounded
  std::vector<std::pair<std::string, std::string>> buf_;
  size_t pos_ = 0;
  size_t chunk_ = 16;  // grows to kCursorChunk
  bool done_ = false;
};

}  // namespace pkql
