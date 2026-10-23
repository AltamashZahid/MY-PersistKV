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
//   r <table id> <encoded pk columns>                 -> encoded non-key columns
//   x <index id> <encoded index cols> <pk>            -> (empty)
//   (ids are 1 byte below 128, see putId in storage.cpp)
//   m t <table name>                                  -> table schema
//   m s <table id u32 BE>                             -> table statistics
//   m n                                               -> next id
//
// Because the encoding is order-preserving, a table's rows are stored in
// primary-key order and an index's entries in index-column order.
// ---------------------------------------------------------------------------

std::string tablePrefix(uint32_t table_id);
size_t keyPrefixLength(std::string_view key);  // length of "<kind><id>" at the start of a row or index key
std::string indexPrefix(uint32_t index_id);
std::string encodeKeyColumns(const Row& row, const std::vector<int>& cols);
std::string rowKey(const TableSchema& t, const Row& row);
std::string rowPayload(const TableSchema& t, const Row& row);
std::string indexKey(const TableSchema& t, const IndexSchema& ix, const Row& row);
// `needed` (one flag per table column) lets the caller skip decoding columns
// the query never reads; they come back as null.
Row decodeStoredRow(const TableSchema& t, std::string_view key, std::string_view payload,
                    const std::vector<bool>* needed = nullptr);
// Same, decoding into `out` and reusing its storage (no allocation once warm).
void decodeStoredRowInto(const TableSchema& t, std::string_view key, std::string_view payload,
                         const std::vector<bool>* needed, Row& out);

// What to decode for a given set of needed columns, worked out once per scan:
// decoding stops after the last needed key column and the last needed
// payload column, so unread trailing columns are never even parsed.
struct DecodePlan {
  size_t ncols = 0;
  // One step per encoded value up to the last needed one: the table column
  // to decode it into, or -1 to skip over it.
  std::vector<int> key_steps;      // primary-key parts, in key order
  std::vector<int> payload_steps;  // payload values, in order
  std::vector<int> unused;         // columns the query never reads (kept null)

  DecodePlan(const TableSchema& t, const std::vector<bool>* needed);
};

void decodeStoredRowInto(const DecodePlan& plan, std::string_view key, std::string_view payload, Row& out);
// The primary key tail of an index entry, turned back into a row key.
std::string rowKeyFromIndexEntry(const TableSchema& t, const IndexSchema& ix, std::string_view entry);

// Appends the row key of every index entry in [lo, hi) to *out, reading the
// entries in place (no per-entry copies). The scan runs in bounded chunks so
// the store's tree lock is never held for long.
void collectRowKeys(pkv::DB& db, const TableSchema& t, const IndexSchema& ix, std::string lo, const std::string& hi,
                    std::vector<std::string>* out);

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

// ---------------------------------------------------------------------------
// RowCursor: table rows in key range [lo, hi), decoded and filtered inside
// PersistKV's zero-copy scan callback. Only rows that pass `pred` are copied
// out, and only the `needed` columns are decoded.
// ---------------------------------------------------------------------------

struct Expr;

class RowCursor {
 public:
  RowCursor(pkv::DB& db, const TableSchema& t, std::string lo, std::string hi_exclusive,
            const std::vector<bool>* needed, const Expr* pred)
      : db_(db), t_(t), next_(std::move(lo)), hi_(std::move(hi_exclusive)), needed_(needed), pred_(pred),
        plan_(t, needed) {}

  bool next(Row* row);

  // Restricts the scan to these row keys (sorted, unique): other rows are
  // passed over without being decoded. Used for sorted index fetches.
  void onlyKeys(std::vector<std::string> keys) {
    keys_ = std::move(keys);
    use_keys_ = true;
  }

 private:
  void fill();

  pkv::DB& db_;
  const TableSchema& t_;
  std::string next_, hi_;
  const std::vector<bool>* needed_;
  const Expr* pred_;
  DecodePlan plan_;
  std::vector<std::string> keys_;
  size_t key_pos_ = 0;
  bool use_keys_ = false;
  std::vector<Row> buf_;  // reused across fills: rows are swapped, not reallocated
  size_t count_ = 0;      // valid rows in buf_
  size_t pos_ = 0;
  size_t chunk_ = 16;  // rows per fill; grows for long scans
  bool done_ = false;
};

}  // namespace pkql
