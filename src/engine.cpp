#include "pkql/engine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <unordered_set>

#include "pkql/codec.h"
#include "pkql/exec.h"

namespace pkql {

namespace {

constexpr size_t kLoadChunk = 2000;
constexpr size_t kHistogramBuckets = 64;
constexpr size_t kAnalyzeSample = 200000;

Result message(std::string m) {
  Result r;
  r.message = std::move(m);
  return r;
}

std::string plural(uint64_t n, const std::string& word) {
  if (n == 1) return "1 " + word;
  if (word.size() > 1 && word.back() == 'y') return std::to_string(n) + " " + word.substr(0, word.size() - 1) + "ies";
  return std::to_string(n) + " " + word + "s";
}

std::string fmtMs(double ms) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.2f ms", ms);
  return buf;
}

Schema tableSchemaColumns(const TableSchema& t, const std::string& qualifier) {
  Schema s;
  for (const Column& c : t.cols) s.push_back({qualifier, c.name, c.type, c.nullable});
  return s;
}

// Reads every row of a table (used by analyze, index builds and drop).
template <typename Fn>
void forEachRow(pkv::DB& db, const TableSchema& t, Fn fn) {
  std::string prefix = tablePrefix(t.id);
  Cursor c(db, prefix, prefixSuccessor(prefix));
  std::string k, v;
  while (c.next(&k, &v)) fn(k, decodeStoredRow(t, k, v));
}

}  // namespace

std::string Result::format() const {
  if (!has_rows) return message;
  std::vector<std::string> head;
  for (const ColumnInfo& c : columns) head.push_back(c.name);
  std::vector<size_t> width;
  for (const std::string& h : head) width.push_back(h.size());
  std::vector<std::vector<std::string>> cells;
  for (const Row& r : rows) {
    std::vector<std::string> line;
    for (size_t i = 0; i < r.size(); i++) {
      line.push_back(r[i].toString());
      if (i < width.size()) width[i] = std::max(width[i], line.back().size());
    }
    cells.push_back(line);
  }
  auto rule = [&] {
    std::string s = "+";
    for (size_t w : width) s += std::string(w + 2, '-') + "+";
    return s + "\n";
  };
  auto line = [&](const std::vector<std::string>& vals) {
    std::string s = "|";
    for (size_t i = 0; i < width.size(); i++) {
      std::string v = i < vals.size() ? vals[i] : "";
      s += " " + v + std::string(width[i] - v.size(), ' ') + " |";
    }
    return s + "\n";
  };
  std::string out = rule() + line(head) + rule();
  for (const auto& l : cells) out += line(l);
  out += rule();
  out += plural(rows.size(), "row");
  if (!message.empty()) out += " (" + message + ")";
  return out;
}

Engine::Engine(const std::string& dir, const pkv::Options& options) : db_(dir, options), catalog_(db_) {
  catalog_.load();
}

std::vector<Result> Engine::run(std::string_view script) {
  std::vector<Result> out;
  for (const Statement& s : parse(script)) out.push_back(execute(s));
  return out;
}

Result Engine::execute(const Statement& s) {
  std::lock_guard<std::mutex> lock(mu_);
  switch (s.kind) {
    case StmtKind::CreateTable: return createTable(s);
    case StmtKind::CreateIndex: return createIndex(s);
    case StmtKind::Insert: return insert(s);
    case StmtKind::Query: return query(s);
    case StmtKind::Let: {
      buildLogicalPlan(*s.pipeline, catalog_, lets_);  // report errors now, not on first use
      if (catalog_.find(s.name)) throw Error("`" + s.name + "` is already a table name");
      lets_[s.name] = s.pipeline;
      return message("defined " + s.name);
    }
    case StmtKind::Analyze: return analyze(s);
    case StmtKind::DropTable: return dropTable(s);
    case StmtKind::DropIndex: return dropIndex(s);
    case StmtKind::ShowTables: return showTables();
    case StmtKind::Describe: return describe(s);
  }
  throw Error("unknown statement");
}

TableSchema& Engine::table(const std::string& name) {
  TableSchema* t = catalog_.find(name);
  if (!t) throw Error("no table named `" + name + "`");
  return *t;
}

// ---------------------------------------------------------------------------
// DDL
// ---------------------------------------------------------------------------

Result Engine::createTable(const Statement& s) {
  if (catalog_.find(s.name)) throw Error("table `" + s.name + "` already exists");
  if (lets_.count(s.name)) throw Error("`" + s.name + "` is already a pipeline name");
  auto t = std::make_unique<TableSchema>();
  t->name = s.name;
  std::set<std::string> names;
  for (const ColumnDef& c : s.cols) {
    if (!names.insert(c.name).second) throw Error("column `" + c.name + "` appears twice");
    if (c.key && c.nullable) throw Error("key column `" + c.name + "` cannot be nullable");
    if (c.key) t->pk.push_back(static_cast<int>(t->cols.size()));
    t->cols.push_back({c.name, c.type, c.nullable});
  }
  if (t->pk.empty()) throw Error("table `" + s.name + "` needs a key column, e.g. `id: int key`");
  pkv::WriteBatch batch;
  t->id = catalog_.allocateId(batch);
  catalog_.stageTable(*t, batch);
  db_.write(batch);
  catalog_.install(std::move(t));
  return message("created table " + s.name);
}

Result Engine::createIndex(const Statement& s) {
  TableSchema& t = table(s.table);
  for (const auto& kv : catalog_.tables()) {
    for (const IndexSchema& ix : kv.second->indexes) {
      if (ix.name == s.name) throw Error("index `" + s.name + "` already exists on " + kv.first);
    }
  }
  IndexSchema ix;
  ix.name = s.name;
  for (const std::string& c : s.index_cols) {
    int i = t.colIndex(c);
    if (i < 0) throw Error("table " + t.name + " has no column `" + c + "`");
    ix.cols.push_back(i);
  }
  pkv::WriteBatch id_batch;
  ix.id = catalog_.allocateId(id_batch);
  db_.write(id_batch);

  // Write the entries for existing rows in chunks; the index only becomes
  // visible when the schema change below commits, so a crash here leaves
  // only unreachable entries.
  pkv::WriteBatch batch;
  uint64_t n = 0;
  forEachRow(db_, t, [&](const std::string&, const Row& row) {
    batch.put(indexKey(t, ix, row), "");
    if (++n % kLoadChunk == 0) {
      db_.write(batch);
      batch.clear();
    }
  });
  db_.write(batch);

  TableSchema updated = t;
  updated.indexes.push_back(ix);
  pkv::WriteBatch schema_batch;
  catalog_.stageTable(updated, schema_batch);
  db_.write(schema_batch);
  t.indexes.push_back(ix);
  return message("created index " + s.name + " on " + t.name + " (" + plural(n, "entry") + ")");
}

Result Engine::dropTable(const Statement& s) {
  TableSchema& t = table(s.name);
  pkv::WriteBatch batch;
  forEachRow(db_, t, [&](const std::string& key, const Row& row) {
    batch.remove(key);
    for (const IndexSchema& ix : t.indexes) batch.remove(indexKey(t, ix, row));
    if (batch.count() >= kLoadChunk) {
      db_.write(batch);
      batch.clear();
    }
  });
  catalog_.stageDrop(t, batch);
  db_.write(batch);
  catalog_.erase(s.name);
  return message("dropped table " + s.name);
}

Result Engine::dropIndex(const Statement& s) {
  for (const auto& kv : catalog_.tables()) {
    TableSchema& t = *kv.second;
    for (size_t i = 0; i < t.indexes.size(); i++) {
      if (t.indexes[i].name != s.name) continue;
      IndexSchema ix = t.indexes[i];
      TableSchema updated = t;
      updated.indexes.erase(updated.indexes.begin() + static_cast<long>(i));
      pkv::WriteBatch batch;
      catalog_.stageTable(updated, batch);
      db_.write(batch);  // the index disappears from the schema first
      t.indexes.erase(t.indexes.begin() + static_cast<long>(i));
      pkv::WriteBatch entries;
      forEachRow(db_, t, [&](const std::string&, const Row& row) {
        entries.remove(indexKey(t, ix, row));
        if (entries.count() >= kLoadChunk) {
          db_.write(entries);
          entries.clear();
        }
      });
      db_.write(entries);
      return message("dropped index " + s.name);
    }
  }
  throw Error("no index named `" + s.name + "`");
}

// ---------------------------------------------------------------------------
// Inserts
// ---------------------------------------------------------------------------

Row Engine::checkRow(const TableSchema& t, Row row) {
  if (row.size() != t.cols.size()) {
    throw Error("table " + t.name + " has " + std::to_string(t.cols.size()) + " columns, got " +
                std::to_string(row.size()));
  }
  for (size_t i = 0; i < row.size(); i++) {
    const Column& c = t.cols[i];
    row[i] = row[i].castTo(c.type);
    if (row[i].isNull()) {
      if (!c.nullable) throw Error("column " + t.name + "." + c.name + " cannot be null");
    } else if (row[i].type() != c.type) {
      throw Error("column " + t.name + "." + c.name + " is " + typeName(c.type) + ", got " +
                  typeName(row[i].type()) + " " + row[i].literal());
    }
  }
  return row;
}

void Engine::stageInsert(TableSchema& t, const std::vector<Row>& rows, pkv::WriteBatch& batch) {
  std::unordered_set<std::string> keys;
  for (const Row& row : rows) {
    std::string key = rowKey(t, row);
    std::string existing;
    if (!keys.insert(key).second || db_.get(key, &existing)) {
      std::string pk;
      for (int c : t.pk) pk += (pk.empty() ? "" : ", ") + row[c].literal();
      throw Error("duplicate key (" + pk + ") in table " + t.name);
    }
    batch.put(key, rowPayload(t, row));
    for (const IndexSchema& ix : t.indexes) batch.put(indexKey(t, ix, row), "");
  }
}

Result Engine::insert(const Statement& s) {
  TableSchema& t = table(s.name);
  std::vector<Row> rows;
  for (const std::vector<NamedExpr>& fields : s.rows) {
    Row row(t.cols.size());
    std::vector<bool> given(t.cols.size(), false);
    for (const NamedExpr& f : fields) {
      int i = t.colIndex(f.name);
      if (i < 0) throw Error("table " + t.name + " has no column `" + f.name + "`");
      if (given[i]) throw Error("column `" + f.name + "` given twice");
      given[i] = true;
      row[i] = eval(*foldConstants(bindExpr(f.expr, Schema())), Row());
    }
    for (size_t i = 0; i < t.cols.size(); i++) {
      if (!given[i] && !t.cols[i].nullable) throw Error("missing value for column " + t.name + "." + t.cols[i].name);
    }
    rows.push_back(checkRow(t, std::move(row)));
  }
  pkv::WriteBatch batch;
  stageInsert(t, rows, batch);
  TableSchema updated = t;
  updated.stats.rows += rows.size();
  catalog_.stageStats(updated, batch);
  db_.write(batch);  // rows, index entries and the row count commit together
  t.stats.rows = updated.stats.rows;
  return message("inserted " + plural(rows.size(), "row"));
}

void Engine::insertRows(const std::string& name, const std::vector<Row>& rows) {
  std::lock_guard<std::mutex> lock(mu_);
  TableSchema& t = table(name);
  for (size_t start = 0; start < rows.size(); start += kLoadChunk) {
    std::vector<Row> chunk;
    for (size_t i = start; i < std::min(rows.size(), start + kLoadChunk); i++) chunk.push_back(checkRow(t, rows[i]));
    pkv::WriteBatch batch;
    stageInsert(t, chunk, batch);
    TableSchema updated = t;
    updated.stats.rows += chunk.size();
    catalog_.stageStats(updated, batch);
    db_.write(batch);
    t.stats.rows = updated.stats.rows;
  }
}

// ---------------------------------------------------------------------------
// Queries, updates and deletes
// ---------------------------------------------------------------------------

Result Engine::query(const Statement& s) {
  const Pipeline& p = *s.pipeline;
  if (!p.steps.empty() && (p.steps.back().kind == StepKind::Delete || p.steps.back().kind == StepKind::Update)) {
    if (s.explain) throw Error("explain works on queries, not on update or delete");
    return modify(s, p);
  }
  LPtr logical = buildLogicalPlan(p, catalog_, lets_);
  PPtr plan = optimize(logical, planner_);

  Result r;
  if (s.explain && !s.explain_analyze) {
    r.message = explainPlan(*plan, false);
    return r;
  }
  ExecContext ctx{db_, s.explain_analyze};
  auto t0 = std::chrono::steady_clock::now();
  std::vector<Row> rows = runPlan(*plan, ctx);
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (s.explain_analyze) {
    r.message = explainPlan(*plan, true) + plural(rows.size(), "row") + " in " + fmtMs(ms);
    return r;
  }
  r.columns = logical->schema;
  r.rows = std::move(rows);
  r.has_rows = true;
  r.message = fmtMs(ms);
  return r;
}

Result Engine::modify(const Statement&, const Pipeline& p) {
  if (p.from.sub || !catalog_.find(p.from.name)) {
    throw Error("update and delete need a table: `from <table> | filter ... | update ...`");
  }
  for (size_t i = 0; i + 1 < p.steps.size(); i++) {
    if (p.steps[i].kind != StepKind::Filter) {
      throw Error("update and delete may only follow `filter` steps");
    }
  }
  TableSchema& t = table(p.from.name);
  const Step& last = p.steps.back();

  // Find the target rows first, then write: rows touched by the update can
  // never be seen again by the same statement (no "Halloween problem").
  Pipeline selection = p;
  selection.steps.pop_back();
  PPtr plan = optimize(buildLogicalPlan(selection, catalog_, lets_), planner_);
  ExecContext ctx{db_, false};
  std::vector<Row> rows = runPlan(*plan, ctx);

  pkv::WriteBatch batch;
  TableSchema updated = t;
  if (last.kind == StepKind::Delete) {
    for (const Row& row : rows) {
      batch.remove(rowKey(t, row));
      for (const IndexSchema& ix : t.indexes) batch.remove(indexKey(t, ix, row));
    }
    updated.stats.rows -= std::min<uint64_t>(updated.stats.rows, rows.size());
    catalog_.stageStats(updated, batch);
    db_.write(batch);
    t.stats.rows = updated.stats.rows;
    return message("deleted " + plural(rows.size(), "row"));
  }

  // Update.
  Schema schema = tableSchemaColumns(t, p.from.alias.empty() ? t.name : p.from.alias);
  std::vector<std::pair<int, ExprPtr>> sets;
  for (const NamedExpr& item : last.items) {
    int c = t.colIndex(item.name);
    if (c < 0) throw Error("table " + t.name + " has no column `" + item.name + "`");
    sets.push_back({c, foldConstants(bindExpr(item.expr, schema))});
  }
  std::vector<Row> new_rows;
  std::unordered_set<std::string> moving;  // old keys of rows whose key changes
  for (const Row& row : rows) {
    Row nr = row;
    for (const auto& s : sets) nr[s.first] = eval(*s.second, row);
    nr = checkRow(t, nr);
    if (rowKey(t, nr) != rowKey(t, row)) moving.insert(rowKey(t, row));
    new_rows.push_back(nr);
  }
  // Remove everything that changes first, then write the new versions, so a
  // row may take over a key another updated row is giving up.
  std::unordered_set<std::string> new_keys;
  for (size_t i = 0; i < rows.size(); i++) {
    std::string ok = rowKey(t, rows[i]), nk = rowKey(t, new_rows[i]);
    if (ok != nk) {
      std::string existing;
      if ((db_.get(nk, &existing) && !moving.count(nk)) || !new_keys.insert(nk).second) {
        throw Error("update would create a duplicate key in table " + t.name);
      }
      batch.remove(ok);
    }
    for (const IndexSchema& ix : t.indexes) {
      std::string oi = indexKey(t, ix, rows[i]), ni = indexKey(t, ix, new_rows[i]);
      if (oi != ni) batch.remove(oi);
    }
  }
  for (size_t i = 0; i < rows.size(); i++) {
    batch.put(rowKey(t, new_rows[i]), rowPayload(t, new_rows[i]));
    for (const IndexSchema& ix : t.indexes) {
      std::string oi = indexKey(t, ix, rows[i]), ni = indexKey(t, ix, new_rows[i]);
      if (oi != ni) batch.put(ni, "");
    }
  }
  db_.write(batch);
  return message("updated " + plural(rows.size(), "row"));
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

void Engine::analyzeTable(TableSchema& t) {
  size_t ncols = t.cols.size();
  std::vector<std::unordered_set<size_t>> distinct(ncols);  // value hashes: an exact-enough ndv
  std::vector<std::vector<Value>> sample(ncols);
  std::vector<uint64_t> nulls(ncols, 0);
  uint64_t rows = 0;
  uint64_t rng = 0x9e3779b97f4a7c15ull;
  forEachRow(db_, t, [&](const std::string&, const Row& row) {
    rows++;
    // Reservoir sampling keeps a uniform sample for the histograms.
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    size_t slot = rows <= kAnalyzeSample ? static_cast<size_t>(rows - 1) : static_cast<size_t>(rng % rows);
    for (size_t c = 0; c < ncols; c++) {
      if (row[c].isNull()) {
        nulls[c]++;
        continue;
      }
      distinct[c].insert(row[c].hash());
      if (sample[c].size() < kAnalyzeSample) sample[c].push_back(row[c]);
      else if (slot < kAnalyzeSample) sample[c][slot] = row[c];
    }
  });

  TableStats st;
  st.rows = rows;
  st.analyzed = true;
  for (size_t c = 0; c < ncols; c++) {
    ColumnStats cs;
    cs.ndv = distinct[c].size();
    cs.nulls = nulls[c];
    std::vector<Value>& v = sample[c];
    std::sort(v.begin(), v.end(), [](const Value& a, const Value& b) { return compare(a, b) < 0; });
    if (!v.empty()) {
      cs.min = v.front();
      cs.max = v.back();
      size_t buckets = std::min(kHistogramBuckets, v.size());
      for (size_t b = 0; b <= buckets; b++) cs.bounds.push_back(v[std::min(v.size() - 1, b * (v.size() - 1) / buckets)]);
    }
    st.cols.push_back(std::move(cs));
  }
  TableSchema updated = t;
  updated.stats = st;
  pkv::WriteBatch batch;
  catalog_.stageStats(updated, batch);
  db_.write(batch);
  t.stats = std::move(st);
}

Result Engine::analyze(const Statement& s) {
  if (!s.name.empty()) {
    analyzeTable(table(s.name));
    return message("analyzed " + s.name);
  }
  for (const auto& kv : catalog_.tables()) analyzeTable(*kv.second);
  return message("analyzed " + plural(catalog_.tables().size(), "table"));
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

Result Engine::showTables() {
  Result r;
  r.has_rows = true;
  r.columns = {{"", "table", Type::Text, false}, {"", "rows", Type::Int, false}, {"", "indexes", Type::Text, false},
               {"", "analyzed", Type::Bool, false}};
  for (const auto& kv : catalog_.tables()) {
    const TableSchema& t = *kv.second;
    std::string ix;
    for (const IndexSchema& i : t.indexes) ix += (ix.empty() ? "" : ", ") + i.name;
    r.rows.push_back({Value::text(t.name), Value::integer(static_cast<int64_t>(t.stats.rows)), Value::text(ix),
                      Value::boolean(t.stats.analyzed)});
  }
  return r;
}

Result Engine::describe(const Statement& s) {
  const TableSchema& t = table(s.name);
  Result r;
  r.has_rows = true;
  r.columns = {{"", "column", Type::Text, false}, {"", "type", Type::Text, false}, {"", "key", Type::Bool, false},
               {"", "nullable", Type::Bool, false}, {"", "distinct", Type::Int, true}};
  for (size_t i = 0; i < t.cols.size(); i++) {
    Value ndv = t.stats.analyzed && i < t.stats.cols.size()
                    ? Value::integer(static_cast<int64_t>(t.stats.cols[i].ndv))
                    : Value::null();
    r.rows.push_back({Value::text(t.cols[i].name), Value::text(typeName(t.cols[i].type)),
                      Value::boolean(t.isPkColumn(static_cast<int>(i))), Value::boolean(t.cols[i].nullable), ndv});
  }
  std::string ix;
  for (const IndexSchema& i : t.indexes) {
    std::string cols;
    for (int c : i.cols) cols += (cols.empty() ? "" : ", ") + t.cols[c].name;
    ix += "\nindex " + i.name + " (" + cols + ")";
  }
  r.message = plural(t.stats.rows, "row") + ix;
  return r;
}

std::string Engine::checkIntegrity() {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& kv : catalog_.tables()) {
    const TableSchema& t = *kv.second;
    uint64_t rows = 0;
    std::string problem;
    forEachRow(db_, t, [&](const std::string&, const Row& row) {
      rows++;
      for (const IndexSchema& ix : t.indexes) {
        std::string v;
        if (problem.empty() && !db_.get(indexKey(t, ix, row), &v)) problem = "row missing from index " + ix.name;
      }
    });
    if (!problem.empty()) return t.name + ": " + problem;
    if (rows != t.stats.rows) {
      return t.name + ": row count is " + std::to_string(t.stats.rows) + " but the table has " + std::to_string(rows);
    }
    for (const IndexSchema& ix : t.indexes) {
      std::string prefix = indexPrefix(ix.id);
      Cursor c(db_, prefix, prefixSuccessor(prefix));
      std::string k, v;
      uint64_t entries = 0;
      while (c.next(&k, &v)) {
        entries++;
        std::string payload;
        if (!db_.get(rowKeyFromIndexEntry(t, ix, k), &payload)) return t.name + ": index " + ix.name + " has a dangling entry";
      }
      if (entries != rows) {
        return t.name + ": index " + ix.name + " has " + std::to_string(entries) + " entries for " +
               std::to_string(rows) + " rows";
      }
    }
  }
  return "";
}

}  // namespace pkql
