#include "pkql/storage.h"

#include <algorithm>

#include "pkql/codec.h"
#include "pkql/colstore.h"
#include "pkql/expr.h"

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
const std::string kFormatKey = "mv";
// 2: compact encodings (varint row values, variable-length integer keys,
// 1-byte object ids). Databases from format 1 cannot be read.
// 3: whole-valued floats stored as varints. 4: dictionary-encoded text.
const std::string kFormatVersion = "4";

std::string schemaKey(const std::string& name) { return "mt" + name; }

std::string dictPrefix(uint32_t table_id) {
  std::string k = "mk";
  putU32(table_id, k);
  return k;
}

std::string dictKey(uint32_t table_id, int col, uint32_t code) {
  std::string k = dictPrefix(table_id);
  k += static_cast<char>((col >> 8) & 0xFF);
  k += static_cast<char>(col & 0xFF);
  putU32(code, k);
  return k;
}

// Column blocks for time-keyed tables.
void attachBlocks(TableSchema& t) {
  if (!t.blocks && blocksEligible(t)) t.blocks = std::make_shared<ColumnBlocks>();
}

// A dictionary for each non-key text column.
void attachDicts(TableSchema& t) {
  t.dicts.resize(t.cols.size());
  for (size_t c = 0; c < t.cols.size(); c++) {
    if (t.cols[c].type == Type::Text && !t.isPkColumn(static_cast<int>(c)) && !t.dicts[c]) {
      t.dicts[c] = std::make_shared<TextDict>();
    }
  }
}

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

namespace {

// Object ids in key prefixes are self-delimiting (the first byte gives the
// length), so small ids cost 1 byte instead of 4: below 0x80 one byte,
// below 0x4000 two bytes, otherwise 0xC0 + 4 bytes.
void putId(uint32_t id, std::string& out) {
  if (id < 0x80) {
    out += static_cast<char>(id);
  } else if (id < 0x4000) {
    out += static_cast<char>(0x80 | (id >> 8));
    out += static_cast<char>(id & 0xFF);
  } else {
    out += static_cast<char>(0xC0);
    putU32(id, out);
  }
}

// Length of the "<kind><id>" prefix at the start of a row or index key.
size_t prefixLength(std::string_view key) {
  if (key.size() < 2) throw Error("corrupt key: missing prefix");
  uint8_t b = static_cast<uint8_t>(key[1]);
  return b < 0x80 ? 2 : b < 0xC0 ? 3 : 6;
}

}  // namespace

size_t keyPrefixLength(std::string_view key) { return prefixLength(key); }

std::string tablePrefix(uint32_t table_id) {
  std::string k = "r";
  putId(table_id, k);
  return k;
}

std::string indexPrefix(uint32_t index_id) {
  std::string k = "x";
  putId(index_id, k);
  return k;
}

std::string encodeKeyColumns(const Row& row, const std::vector<int>& cols) {
  std::string out;
  for (int c : cols) encodeKey(row[c], out);
  return out;
}

// Keys and payloads are built in one string each (these run once per row
// and index entry on every insert).
std::string rowKey(const TableSchema& t, const Row& row) {
  std::string k;
  k.reserve(24);
  k += 'r';
  putId(t.id, k);
  for (int c : t.pk) encodeKey(row[c], k);
  return checkedKey(std::move(k));
}

std::string rowPayload(const TableSchema& t, const Row& row) {
  std::string out;
  out.reserve(8 * t.cols.size());
  for (size_t i = 0; i < t.cols.size(); i++) {
    if (!t.isPkColumn(static_cast<int>(i))) encodeValue(row[i], out, t.dict(static_cast<int>(i)));
  }
  return out;
}

std::string indexKey(const TableSchema& t, const IndexSchema& ix, const Row& row) {
  std::string k;
  k.reserve(32);
  k += 'x';
  putId(ix.id, k);
  for (int c : ix.cols) encodeKey(row[c], k);
  for (int c : t.pk) encodeKey(row[c], k);
  return checkedKey(std::move(k));
}

void decodeStoredRowInto(const TableSchema& t, std::string_view key, std::string_view payload,
                         const std::vector<bool>* needed, Row& row) {
  row.resize(t.cols.size());
  key.remove_prefix(prefixLength(key));  // 'r' + table id
  for (int c : t.pk) {
    Value v = decodeKey(key);
    row[c] = (!needed || (*needed)[c]) ? std::move(v) : Value();
  }
  // The payload holds the non-key columns in table order.
  for (size_t i = 0; i < t.cols.size(); i++) {
    if (t.isPkColumn(static_cast<int>(i))) continue;
    if (payload.empty()) throw Error("corrupt row in table " + t.name);
    bool keep = !needed || (*needed)[i];
    if (!keep) row[i] = Value();
    decodeValue(payload, keep ? &row[i] : nullptr, t.dict(static_cast<int>(i)));
  }
}

DecodePlan::DecodePlan(const TableSchema& t, const std::vector<bool>* needed) : ncols(t.cols.size()) {
  auto want = [&](int c) { return !needed || (*needed)[c]; };
  size_t last = 0;
  for (int c : t.pk) {
    key_steps.push_back(want(c) ? c : -1);
    if (want(c)) last = key_steps.size();
  }
  key_steps.resize(last);  // stop after the last needed key part
  last = 0;
  for (size_t i = 0; i < t.cols.size(); i++) {
    if (t.isPkColumn(static_cast<int>(i))) continue;
    payload_steps.push_back(want(static_cast<int>(i)) ? static_cast<int>(i) : -1);
    payload_dicts.push_back(t.dict(static_cast<int>(i)));
    if (want(static_cast<int>(i))) last = payload_steps.size();
  }
  payload_steps.resize(last);  // stop after the last needed payload value
  payload_dicts.resize(last);
  for (size_t i = 0; i < t.cols.size(); i++) {
    if (!want(static_cast<int>(i))) unused.push_back(static_cast<int>(i));
  }
}

void decodeStoredRowInto(const DecodePlan& plan, std::string_view key, std::string_view payload, Row& row) {
  if (row.size() != plan.ncols) row.assign(plan.ncols, Value());
  key.remove_prefix(prefixLength(key));  // 'r' + table id
  for (int c : plan.key_steps) decodeKeyInto(key, c >= 0 ? &row[c] : nullptr);
  for (size_t s = 0; s < plan.payload_steps.size(); s++) {
    int c = plan.payload_steps[s];
    if (payload.empty()) throw Error("corrupt row");
    decodeValue(payload, c >= 0 ? &row[c] : nullptr, plan.payload_dicts[s]);
  }
  for (int c : plan.unused) row[c].setNull();  // columns the query never reads stay null
}

Row decodeStoredRow(const TableSchema& t, std::string_view key, std::string_view payload,
                    const std::vector<bool>* needed) {
  Row row;
  decodeStoredRowInto(t, key, payload, needed, row);
  return row;
}

std::string rowKeyFromIndexEntry(const TableSchema& t, const IndexSchema& ix, std::string_view entry) {
  entry.remove_prefix(prefixLength(entry));  // 'x' + index id
  for (size_t i = 0; i < ix.cols.size(); i++) decodeKey(entry);  // skip the indexed columns
  return tablePrefix(t.id) + std::string(entry);
}

void collectRowKeys(pkv::DB& db, const TableSchema& t, const IndexSchema& ix, std::string lo, const std::string& hi,
                    std::vector<std::string>* out) {
  const std::string table_prefix = tablePrefix(t.id);
  constexpr size_t kChunk = 4096;
  while (true) {
    size_t n = 0;
    bool more = false;
    std::string last;
    db.scanRaw(lo, "", [&](const char* k, size_t kl, const char*, size_t) {
      if (!hi.empty() && hi.compare(0, hi.size(), k, kl) <= 0) return false;
      std::string_view entry(k, kl);
      entry.remove_prefix(prefixLength(entry));  // 'x' + index id
      for (size_t i = 0; i < ix.cols.size(); i++) decodeKeyInto(entry, nullptr);  // skip the indexed columns
      std::string& rk = out->emplace_back();
      rk.reserve(table_prefix.size() + entry.size());
      rk.append(table_prefix).append(entry);
      if (++n < kChunk) return true;
      last.assign(k, kl);
      more = true;
      return false;
    });
    if (!more) return;
    lo = last + '\0';  // smallest key after the last one
  }
}

void sortKeys(std::vector<std::string>& keys) {
  if (std::is_sorted(keys.begin(), keys.end())) return;
  // Zero-padding a short key's prefix keeps prefix order consistent with
  // bytewise order; equal prefixes fall back to the full comparison.
  std::vector<std::pair<uint64_t, uint32_t>> order(keys.size());
  for (size_t i = 0; i < keys.size(); i++) {
    uint64_t prefix = 0;
    for (size_t j = 0; j < 8; j++) {
      prefix = (prefix << 8) | (j < keys[i].size() ? static_cast<uint8_t>(keys[i][j]) : 0);
    }
    order[i] = {prefix, static_cast<uint32_t>(i)};
  }
  std::sort(order.begin(), order.end(), [&](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first < b.first;
    return keys[a.second] < keys[b.second];
  });
  std::vector<std::string> sorted;
  sorted.reserve(keys.size());
  for (const auto& o : order) sorted.push_back(std::move(keys[o.second]));
  keys.swap(sorted);
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
  std::string format;
  if (!blobs.empty() && (!db_.get(kFormatKey, &format) || format != kFormatVersion)) {
    throw Error("this database was created by an older PKQL storage format; re-create it");
  }
  for (const std::string& blob : blobs) {
    auto t = std::make_unique<TableSchema>(deserializeSchema(blob));
    std::string stats;
    if (db_.get(statsKey(t->id), &stats)) t->stats = deserializeStats(stats);
    attachDicts(*t);
    // Dictionary entries come back in code order (the key ends in the code).
    std::string prefix = dictPrefix(t->id);
    db_.scan(prefix, "", [&](const std::string& k, const std::string& val) {
      if (k.compare(0, prefix.size(), prefix) != 0) return false;
      if (k.size() != prefix.size() + 6) throw Error("corrupt dictionary key");
      int col = (static_cast<uint8_t>(k[prefix.size()]) << 8) | static_cast<uint8_t>(k[prefix.size() + 1]);
      TextDict* d = t->dict(col);
      if (!d) throw Error("dictionary for a column without one in table " + t->name);
      d->codes.emplace(val, static_cast<uint32_t>(d->values.size()));
      d->values.push_back(val);
      return true;
    });
    attachBlocks(*t);
    if (t->blocks) {
      std::string bprefix = blockMetaPrefix(t->id);
      db_.scan(bprefix, "", [&](const std::string& k, const std::string& val) {
        if (k.compare(0, bprefix.size(), bprefix) != 0) return false;
        BlockMeta m = deserializeBlockMeta(val, firstRowKeyFromMetaKey(*t, k));
        t->blocks->covered_rows += m.rows;
        std::string first = m.first_key;
        t->blocks->by_first.emplace(std::move(first), std::move(m));
        return true;
      });
    }
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
  batch.put(kFormatKey, kFormatVersion);
  batch.put(schemaKey(t.name), serializeSchema(t));
  stageStats(t, batch);
}

void Catalog::stageStats(const TableSchema& t, pkv::WriteBatch& batch) {
  batch.put(statsKey(t.id), serializeStats(t.stats));
}

void Catalog::stageDrop(const TableSchema& t, pkv::WriteBatch& batch) {
  batch.remove(schemaKey(t.name));
  batch.remove(statsKey(t.id));
  for (size_t c = 0; c < t.dicts.size(); c++) {
    if (!t.dicts[c]) continue;
    for (size_t code = 0; code < t.dicts[c]->values.size(); code++) {
      batch.remove(dictKey(t.id, static_cast<int>(c), static_cast<uint32_t>(code)));
    }
  }
  if (t.blocks) {
    for (const auto& b : t.blocks->by_first) {
      batch.remove(blockKey(t, b.first));
      batch.remove(blockMetaKey(t, b.first));
    }
  }
}

DictTxn::DictTxn(const TableSchema& t) : t_(t) {
  for (const auto& d : t.dicts) marks_.push_back(d ? d->values.size() : 0);
}

DictTxn::~DictTxn() {
  if (committed_) return;
  for (size_t c = 0; c < t_.dicts.size() && c < marks_.size(); c++) {
    if (t_.dicts[c]) t_.dicts[c]->truncate(marks_[c]);
  }
}

void DictTxn::stage(pkv::WriteBatch& batch) const {
  for (size_t c = 0; c < t_.dicts.size() && c < marks_.size(); c++) {
    const TextDict* d = t_.dicts[c].get();
    if (!d) continue;
    for (size_t code = marks_[c]; code < d->values.size(); code++) {
      batch.put(dictKey(t_.id, static_cast<int>(c), static_cast<uint32_t>(code)), d->values[code]);
    }
  }
}

void Catalog::install(std::unique_ptr<TableSchema> t) {
  attachDicts(*t);
  attachBlocks(*t);
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
  if (reverse_) {
    db_.scanRawReverse(next_, hi_, [&](const char* k, size_t kl, const char* v, size_t vl) {
      buf_.emplace_back(std::string(k, kl), std::string(v, vl));
      return buf_.size() < chunk;
    });
    if (buf_.size() < chunk) done_ = true;
    if (!buf_.empty()) hi_ = buf_.back().first;  // the next chunk ends just below the last key
    return;
  }
  db_.scanRaw(next_, "", [&](const char* k, size_t kl, const char* v, size_t vl) {
    if (!hi_.empty() && hi_.compare(0, hi_.size(), k, kl) <= 0) {
      done_ = true;
      return false;
    }
    buf_.emplace_back(std::string(k, kl), std::string(v, vl));
    return buf_.size() < chunk;
  });
  if (buf_.size() < chunk) done_ = true;  // the scan ran out of keys
  if (!buf_.empty()) next_ = buf_.back().first + '\0';  // smallest key after the last one
}

void RowCursor::fill() {
  count_ = 0;
  pos_ = 0;
  if (done_) return;
  size_t want = chunk_;
  chunk_ = std::min<size_t>(chunk_ * 4, 1024);
  if (reverse_) {
    // Newest-first: the same decode and filter, with the upper bound moving
    // down to the last key looked at.
    size_t scanned = 0, max_scan = std::max<size_t>(want * 8, 4096);
    std::string last;
    bool stopped_early = false;
    db_.scanRawReverse(next_, hi_, [&](const char* k, size_t kl, const char* v, size_t vl) {
      if (count_ == buf_.size()) buf_.emplace_back();
      Row& row = buf_[count_];
      decodeStoredRowInto(plan_, std::string_view(k, kl), std::string_view(v, vl), row);
      if (!pred_ || isTrue(eval(*pred_, row))) count_++;
      if (count_ >= want || ++scanned >= max_scan) {
        last.assign(k, kl);
        stopped_early = true;
        return false;
      }
      return true;
    });
    if (stopped_early) hi_ = last;  // exclusive: resume just below it
    else done_ = true;
    return;
  }
  // Bound the work per scan call (the scan holds PersistKV's tree lock),
  // even when the filter rejects almost every row.
  size_t scanned = 0, max_scan = std::max<size_t>(want * 8, 4096);
  std::string last;
  bool stopped_early = false;
  db_.scanRaw(next_, "", [&](const char* k, size_t kl, const char* v, size_t vl) {
    if (!hi_.empty() && hi_.compare(0, hi_.size(), k, kl) <= 0) {
      done_ = true;
      return false;
    }
    if (use_keys_) {
      // Merge against the sorted key list: skip rows that are not on it.
      while (key_pos_ < keys_.size() && keys_[key_pos_].compare(0, std::string::npos, k, kl) < 0) key_pos_++;
      if (key_pos_ == keys_.size()) {
        done_ = true;
        return false;
      }
      bool wanted = keys_[key_pos_].compare(0, std::string::npos, k, kl) == 0;
      if (!wanted) {
        if (++scanned >= max_scan) {
          last.assign(k, kl);
          stopped_early = true;
          return false;
        }
        return true;
      }
      key_pos_++;
    }
    if (count_ == buf_.size()) buf_.emplace_back();
    Row& row = buf_[count_];
    decodeStoredRowInto(plan_, std::string_view(k, kl), std::string_view(v, vl), row);
    if (!pred_ || isTrue(eval(*pred_, row))) count_++;
    if (count_ >= want || ++scanned >= max_scan) {
      last.assign(k, kl);
      stopped_early = true;
      return false;
    }
    return true;
  });
  if (stopped_early) next_ = last + '\0';  // resume right after the last key looked at
  else done_ = true;                       // the scan reached the end of the store
}

bool RowCursor::next(Row* row) {
  while (pos_ >= count_) {
    if (done_) return false;
    fill();
  }
  std::swap(*row, buf_[pos_++]);  // hand the row over; take the caller's old buffer back
  return true;
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
