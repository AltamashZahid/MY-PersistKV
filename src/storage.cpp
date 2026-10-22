#include "pkql/storage.h"

#include "pkql/codec.h"

namespace pkql {

namespace {

constexpr size_t kMaxKeyBytes = pkv::kMaxKeySize;
constexpr size_t kCursorChunk = 1024;

void putU32(uint32_t x, std::string& out) {
  for (int shift = 24; shift >= 0; shift -= 8) out += static_cast<char>((x >> shift) & 0xFF);
}

std::string checkedKey(std::string key) {
  if (key.size() > kMaxKeyBytes) {
    throw Error("key too long: " + std::to_string(key.size()) + " bytes (the storage engine allows " +
                std::to_string(kMaxKeyBytes) + ")");
  }
  return key;
}

const std::string kNextIdKey = "mn";

std::string schemaKey(const std::string& name) { return "mt" + name; }

std::string statsKey(uint32_t id) {
  std::string k = "ms";
  putU32(id, k);
  return k;
}

// Schemas and statistics are serialized as a flat Row with the row codec.
struct RowWriter {
  Row r;
  void i(int64_t x) { r.push_back(Value::integer(x)); }
  void s(const std::string& x) { r.push_back(Value::text(x)); }
  void v(const Value& x) { r.push_back(x); }
};

struct RowReader {
  const Row& r;
  size_t p = 0;
  const Value& next() {
    if (p >= r.size()) throw Error("corrupt catalog entry");
    return r[p++];
  }
  int64_t i() { return next().asInt(); }
  std::string s() { return next().asText(); }
  Value v() { return next(); }
};

std::string serializeSchema(const TableSchema& t) {
  RowWriter w;
  w.i(t.id);
  w.s(t.name);
  w.i(static_cast<int64_t>(t.cols.size()));
  for (const Column& c : t.cols) {
    w.s(c.name);
    w.i(static_cast<int64_t>(c.type));
    w.i(c.nullable);
  }
  w.i(static_cast<int64_t>(t.pk.size()));
  for (int c : t.pk) w.i(c);
  w.i(static_cast<int64_t>(t.indexes.size()));
  for (const IndexSchema& ix : t.indexes) {
    w.i(ix.id);
    w.s(ix.name);
    w.i(static_cast<int64_t>(ix.cols.size()));
    for (int c : ix.cols) w.i(c);
  }
  std::string out;
  encodeRow(w.r, out);
  return out;
}

TableSchema deserializeSchema(std::string_view bytes) {
  Row row = decodeRow(bytes);
  RowReader r{row};
  TableSchema t;
  t.id = static_cast<uint32_t>(r.i());
  t.name = r.s();
  for (int64_t n = r.i(); n > 0; n--) {
    Column c;
    c.name = r.s();
    c.type = static_cast<Type>(r.i());
    c.nullable = r.i() != 0;
    t.cols.push_back(c);
  }
  for (int64_t n = r.i(); n > 0; n--) t.pk.push_back(static_cast<int>(r.i()));
  for (int64_t n = r.i(); n > 0; n--) {
    IndexSchema ix;
    ix.id = static_cast<uint32_t>(r.i());
    ix.name = r.s();
    for (int64_t m = r.i(); m > 0; m--) ix.cols.push_back(static_cast<int>(r.i()));
    t.indexes.push_back(ix);
  }
  return t;
}

std::string serializeStats(const TableStats& s) {
  RowWriter w;
  w.i(static_cast<int64_t>(s.rows));
  w.i(s.analyzed);
  w.i(static_cast<int64_t>(s.cols.size()));
  for (const ColumnStats& c : s.cols) {
    w.i(static_cast<int64_t>(c.ndv));
    w.i(static_cast<int64_t>(c.nulls));
    w.v(c.min);
    w.v(c.max);
    w.i(static_cast<int64_t>(c.bounds.size()));
    for (const Value& b : c.bounds) w.v(b);
  }
  std::string out;
  encodeRow(w.r, out);
  return out;
}

TableStats deserializeStats(std::string_view bytes) {
  Row row = decodeRow(bytes);
  RowReader r{row};
  TableStats s;
  s.rows = static_cast<uint64_t>(r.i());
  s.analyzed = r.i() != 0;
  for (int64_t n = r.i(); n > 0; n--) {
    ColumnStats c;
    c.ndv = static_cast<uint64_t>(r.i());
    c.nulls = static_cast<uint64_t>(r.i());
    c.min = r.v();
    c.max = r.v();
    for (int64_t m = r.i(); m > 0; m--) c.bounds.push_back(r.v());
    s.cols.push_back(std::move(c));
  }
  return s;
}

}  // namespace

int TableSchema::colIndex(const std::string& col) const {
  for (size_t i = 0; i < cols.size(); i++) {
    if (cols[i].name == col) return static_cast<int>(i);
  }
  return -1;
}

bool TableSchema::isPkColumn(int col) const {
  for (int c : pk) {
    if (c == col) return true;
  }
  return false;
}

std::string tablePrefix(uint32_t table_id) {
  std::string k = "r";
  putU32(table_id, k);
  return k;
}

std::string indexPrefix(uint32_t index_id) {
  std::string k = "x";
  putU32(index_id, k);
  return k;
}

std::string encodeKeyColumns(const Row& row, const std::vector<int>& cols) {
  std::string out;
  for (int c : cols) encodeKey(row[c], out);
  return out;
}

std::string rowKey(const TableSchema& t, const Row& row) {
  return checkedKey(tablePrefix(t.id) + encodeKeyColumns(row, t.pk));
}

std::string rowPayload(const TableSchema& t, const Row& row) {
  Row rest;
  for (size_t i = 0; i < t.cols.size(); i++) {
    if (!t.isPkColumn(static_cast<int>(i))) rest.push_back(row[i]);
  }
  std::string out;
  encodeRow(rest, out);
  return out;
}

std::string indexKey(const TableSchema& t, const IndexSchema& ix, const Row& row) {
  return checkedKey(indexPrefix(ix.id) + encodeKeyColumns(row, ix.cols) + encodeKeyColumns(row, t.pk));
}

Row decodeStoredRow(const TableSchema& t, std::string_view key, std::string_view payload,
                    const std::vector<bool>* needed) {
  Row row(t.cols.size());
  key.remove_prefix(5);  // 'r' + table id
  for (int c : t.pk) {
    Value v = decodeKey(key);
    if (!needed || (*needed)[c]) row[c] = std::move(v);
  }
  // The payload holds the non-key columns in table order.
  for (size_t i = 0; i < t.cols.size(); i++) {
    if (t.isPkColumn(static_cast<int>(i))) continue;
    if (payload.empty()) throw Error("corrupt row in table " + t.name);
    decodeValue(payload, needed && !(*needed)[i] ? nullptr : &row[i]);
  }
  return row;
}

std::string rowKeyFromIndexEntry(const TableSchema& t, const IndexSchema& ix, std::string_view entry) {
  entry.remove_prefix(5);  // 'x' + index id
  for (size_t i = 0; i < ix.cols.size(); i++) decodeKey(entry);  // skip the indexed columns
  return tablePrefix(t.id) + std::string(entry);
}

// ---------------------------------------------------------------------------

void Catalog::load() {
  tables_.clear();
  std::string v;
  if (db_.get(kNextIdKey, &v)) next_id_ = static_cast<uint32_t>(std::stoul(v));

  std::vector<std::string> blobs;
  db_.scan("mt", "", [&](const std::string& k, const std::string& val) {
    if (k.compare(0, 2, "mt") != 0) return false;
    blobs.push_back(val);
    return true;
  });
  for (const std::string& blob : blobs) {
    auto t = std::make_unique<TableSchema>(deserializeSchema(blob));
    std::string stats;
    if (db_.get(statsKey(t->id), &stats)) t->stats = deserializeStats(stats);
    std::string name = t->name;
    tables_[name] = std::move(t);
  }
}

TableSchema* Catalog::find(const std::string& name) {
  auto it = tables_.find(name);
  return it == tables_.end() ? nullptr : it->second.get();
}

uint32_t Catalog::allocateId(pkv::WriteBatch& batch) {
  uint32_t id = next_id_++;
  batch.put(kNextIdKey, std::to_string(next_id_));
  return id;
}

void Catalog::stageTable(const TableSchema& t, pkv::WriteBatch& batch) {
  batch.put(schemaKey(t.name), serializeSchema(t));
  stageStats(t, batch);
}

void Catalog::stageStats(const TableSchema& t, pkv::WriteBatch& batch) {
  batch.put(statsKey(t.id), serializeStats(t.stats));
}

void Catalog::stageDrop(const TableSchema& t, pkv::WriteBatch& batch) {
  batch.remove(schemaKey(t.name));
  batch.remove(statsKey(t.id));
}

void Catalog::install(std::unique_ptr<TableSchema> t) {
  std::string name = t->name;
  tables_[name] = std::move(t);
}

void Catalog::erase(const std::string& name) { tables_.erase(name); }

// ---------------------------------------------------------------------------

void Cursor::fill() {
  buf_.clear();
  pos_ = 0;
  if (done_) return;
  // Small chunks first so `take 10` over a scan reads little; larger ones
  // for long scans.
  size_t chunk = chunk_;
  chunk_ = std::min(chunk_ * 4, kCursorChunk);
  db_.scan(next_, "", [&](const std::string& k, const std::string& v) {
    if (!hi_.empty() && k >= hi_) {
      done_ = true;
      return false;
    }
    buf_.emplace_back(k, v);
    return buf_.size() < chunk;
  });
  if (buf_.size() < chunk) done_ = true;  // the scan ran out of keys
  if (!buf_.empty()) next_ = buf_.back().first + '\0';  // smallest key after the last one
}

bool Cursor::next(std::string* key, std::string* value) {
  if (pos_ >= buf_.size()) {
    fill();
    if (buf_.empty()) return false;
  }
  *key = std::move(buf_[pos_].first);
  *value = std::move(buf_[pos_].second);
  pos_++;
  return true;
}

}  // namespace pkql
