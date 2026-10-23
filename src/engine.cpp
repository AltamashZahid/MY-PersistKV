#include "pkql/engine.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <unordered_set>

#include "pkql/codec.h"
#include "pkql/exec.h"
#include "pkql/sketch.h"

namespace pkql {

namespace {

constexpr size_t kLoadChunk = 2000;
constexpr size_t kHistogramBuckets = 64;
// Rows sampled for histograms: 30,000, as PostgreSQL's default (300 x its
// statistics target) for a similar bucket count. Larger samples barely
// change the bounds but copy and sort far more values.
constexpr size_t kAnalyzeSample = 30000;

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

namespace {

void collectTables(const PNode& p, std::vector<const TableSchema*>& out) {
  if (p.table) out.push_back(p.table);
  for (const PPtr& k : p.kids) collectTables(*k, out);
}

void patchExpr(const ExprPtr& e, const std::vector<Value>& params, int64_t now) {
  if (!e) return;
  if (e->kind == ExprKind::Literal && e->now) e->value = Value::integer(now + e->now_offset);
  if (e->kind == ExprKind::Literal && e->param >= 0 && e->param < static_cast<int>(params.size())) {
    Value v = params[e->param];
    if (e->value.type() == Type::Float && v.type() == Type::Int) v = v.castTo(Type::Float);
    e->value = std::move(v);
  }
  for (const ExprPtr& a : e->args) patchExpr(a, params, now);
}

// Puts this execution's literal values into a cached plan.
void patchPlan(PNode& p, const std::vector<Value>& params, int64_t now) {
  for (const ExprPtr& e : p.eq) patchExpr(e, params, now);
  patchExpr(p.lo, params, now);
  patchExpr(p.hi, params, now);
  patchExpr(p.pred, params, now);
  for (const ExprPtr& e : p.left_keys) patchExpr(e, params, now);
  for (const ExprPtr& e : p.right_keys) patchExpr(e, params, now);
  for (const ExprPtr& e : p.exprs) patchExpr(e, params, now);
  for (const AggSpec& a : p.aggs) patchExpr(a.arg, params, now);
  for (const SortSpec& s : p.sort) patchExpr(s.expr, params, now);
  for (const PPtr& k : p.kids) patchPlan(*k, params, now);
}

}  // namespace

std::vector<Result> Engine::run(std::string_view script) {
  std::vector<Result> out;
  Result fast;
  if (runFast(script, &fast)) {
    out.push_back(std::move(fast));
    return out;
  }
  for (const Statement& s : parse(script)) out.push_back(execute(s));
  return out;
}

// A repeated query shape whose plan is cached: lex, look up the skeleton,
// check the non-parameter literals, and run, without building a syntax tree.
bool Engine::runFast(std::string_view text, Result* out) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!plan_cache_on_ || skeletons_.empty()) return false;
  std::string skeleton;
  std::vector<Value> literals;
  if (!skeletonOf(text, &skeleton, &literals)) return false;
  skeleton += std::to_string(planner_.reorder_joins) + std::to_string(planner_.use_indexes) +
              std::to_string(planner_.use_interesting_orders) + std::to_string(planner_.vectorize);
  auto sk = skeletons_.find(skeleton);
  if (sk == skeletons_.end()) return false;
  const Skeleton& s = sk->second;
  for (size_t i = 0; i < s.fixed.size(); i++) {
    if (literals[s.fixed[i]] != s.fixed_values[i] || literals[s.fixed[i]].type() != s.fixed_values[i].type()) return false;
  }
  auto pc = plan_cache_.find(s.plan_key);
  if (pc == plan_cache_.end() || !cacheValid(pc->second)) return false;
  std::vector<Value> params;
  for (size_t i = 0; i < s.param_literal.size(); i++) {
    Value v = literals[s.param_literal[i]];
    if (s.param_negated[i]) v = v.type() == Type::Int ? Value::integer(-v.asInt()) : Value::real(-v.asFloat());
    params.push_back(std::move(v));
  }
  cache_hits_++;
  *out = runCached(pc->second, params);
  return true;
}

Result Engine::runCached(CachedPlan& c, const std::vector<Value>& params) {
  patchPlan(*c.plan, params, nowMillis());
  auto t0 = std::chrono::steady_clock::now();
  Result r;
  if (!c.op) c.op = buildOperator(*c.plan, exec_ctx_);
  c.op->open();
  Row row;
  while (c.op->next(&row)) r.rows.push_back(row);
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  r.columns = c.columns;
  r.has_rows = true;
  r.message = fmtMs(ms);
  return r;
}

Result Engine::execute(const Statement& s) {
  std::lock_guard<std::mutex> lock(mu_);
  if (s.kind != StmtKind::Query && s.kind != StmtKind::Insert && s.kind != StmtKind::ShowTables &&
      s.kind != StmtKind::Describe) {
    catalog_version_++;  // schema, statistics or let definitions may change
  }
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

// True when the row can be stored as it is: right width, exact types, and no
// null in a non-null column. Such rows need no checked copy.
bool Engine::rowFits(const TableSchema& t, const Row& row) const {
  if (row.size() != t.cols.size()) return false;
  for (size_t i = 0; i < row.size(); i++) {
    if (row[i].isNull() ? !t.cols[i].nullable : row[i].type() != t.cols[i].type) return false;
  }
  return true;
}

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

void Engine::stageInsert(TableSchema& t, const std::vector<const Row*>& rows, pkv::WriteBatch& batch) {
  std::vector<std::string> keys(rows.size());
  for (size_t i = 0; i < rows.size(); i++) keys[i] = rowKey(t, *rows[i]);

  // Primary keys must be new. Sorting finds repeats within the statement, and
  // one batched lookup checks the store: for appends (ascending ids) the
  // keys are already sorted and all land past the last leaf, so this is cheap.
  auto duplicate = [&](const std::string& key) {
    std::string_view k(key);
    k.remove_prefix(keyPrefixLength(k));
    std::string pk;
    for (size_t c = 0; c < t.pk.size(); c++) pk += (pk.empty() ? "" : ", ") + decodeKey(k).literal();
    return Error("duplicate key (" + pk + ") in table " + t.name);
  };
  std::vector<std::string> sorted = keys;
  sortKeys(sorted);
  for (size_t i = 1; i < sorted.size(); i++) {
    if (sorted[i] == sorted[i - 1]) throw duplicate(sorted[i]);
  }
  const std::string* existing = nullptr;
  db_.multiGetRaw(sorted, [&](size_t i, const char*, size_t) {
    if (!existing) existing = &sorted[i];
  });
  if (existing) throw duplicate(*existing);

  batch.reserve(rows.size() * (1 + t.indexes.size()));
  for (size_t i = 0; i < rows.size(); i++) {
    batch.put(std::move(keys[i]), rowPayload(t, *rows[i]));
    for (const IndexSchema& ix : t.indexes) batch.put(indexKey(t, ix, *rows[i]), std::string());
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
  std::vector<const Row*> ptrs;
  for (const Row& r : rows) ptrs.push_back(&r);
  DictTxn dicts(t);
  stageInsert(t, ptrs, batch);
  TableSchema updated = t;
  updated.stats.rows += rows.size();
  catalog_.stageStats(updated, batch);
  dicts.stage(batch);
  db_.write(batch);  // rows, index entries, new dictionary codes and the row count commit together
  dicts.commit();
  t.stats.rows = updated.stats.rows;
  return message("inserted " + plural(rows.size(), "row"));
}

void Engine::insertRows(const std::string& name, const std::vector<Row>& rows) {
  std::lock_guard<std::mutex> lock(mu_);
  TableSchema& t = table(name);
  for (size_t start = 0; start < rows.size(); start += kLoadChunk) {
    // Rows that already fit the schema are stored from the caller's vector;
    // only rows that need a cast (or fail a check) are copied.
    size_t end = std::min(rows.size(), start + kLoadChunk);
    std::vector<Row> fixed;
    fixed.reserve(end - start);
    std::vector<const Row*> chunk;
    chunk.reserve(end - start);
    for (size_t i = start; i < end; i++) {
      if (rowFits(t, rows[i])) {
        chunk.push_back(&rows[i]);
      } else {
        fixed.push_back(checkRow(t, rows[i]));
        chunk.push_back(&fixed.back());
      }
    }
    pkv::WriteBatch batch;
    DictTxn dicts(t);
    stageInsert(t, chunk, batch);
    TableSchema updated = t;
    updated.stats.rows += chunk.size();
    catalog_.stageStats(updated, batch);
    dicts.stage(batch);
    db_.write(batch);
    dicts.commit();
    t.stats.rows = updated.stats.rows;
  }
}

// ---------------------------------------------------------------------------
// Queries, updates and deletes
// ---------------------------------------------------------------------------



bool Engine::cacheValid(const CachedPlan& c) const {
  if (c.version != catalog_version_) return false;
  // Re-plan when a table grew or shrank a lot since the plan was made.
  for (const auto& tr : c.rows_when_planned) {
    double now = static_cast<double>(tr.first->stats.rows), then = static_cast<double>(tr.second);
    if (std::abs(now - then) > 64 && (now > 2 * then || now < then / 2)) return false;
  }
  return true;
}

Result Engine::query(const Statement& s) {
  const Pipeline& p = *s.pipeline;
  if (!p.steps.empty() && (p.steps.back().kind == StepKind::Delete || p.steps.back().kind == StepKind::Update)) {
    if (s.explain) throw Error("explain works on queries, not on update or delete");
    return modify(s, p);
  }
  PPtr plan;
  Schema columns;
  std::string key;
  if (plan_cache_on_ && !s.explain && !s.fingerprint.empty()) {
    key = s.fingerprint + std::to_string(planner_.reorder_joins) + std::to_string(planner_.use_indexes) +
          std::to_string(planner_.use_interesting_orders) + std::to_string(planner_.vectorize);
    auto it = plan_cache_.find(key);
    if (it != plan_cache_.end() && cacheValid(it->second)) {
      cache_hits_++;
      return runCached(it->second, s.params);
    }
  }
  if (!plan) {
    LPtr logical = buildLogicalPlan(p, catalog_, lets_);
    plan = optimize(logical, planner_);
    columns = logical->schema;
    if (!key.empty()) {
      cache_misses_++;
      if (plan_cache_.size() >= 1024) plan_cache_.clear();
      CachedPlan c;
      c.plan = plan;
      c.columns = columns;
      c.version = catalog_version_;
      std::vector<const TableSchema*> tables;
      collectTables(*plan, tables);
      for (const TableSchema* t : tables) c.rows_when_planned.push_back({t, t->stats.rows});
      plan_cache_[key] = std::move(c);
      if (!s.skeleton.empty()) {
        Skeleton sk;
        sk.plan_key = key;
        sk.param_literal = s.param_literal;
        sk.param_negated = s.param_negated;
        for (size_t i = 0; i < s.literals.size(); i++) {
          if (std::find(s.param_literal.begin(), s.param_literal.end(), static_cast<int>(i)) == s.param_literal.end()) {
            sk.fixed.push_back(static_cast<int>(i));
            sk.fixed_values.push_back(s.literals[i]);
          }
        }
        if (skeletons_.size() >= 1024) skeletons_.clear();
        skeletons_[s.skeleton + std::to_string(planner_.reorder_joins) + std::to_string(planner_.use_indexes) +
                   std::to_string(planner_.use_interesting_orders) + std::to_string(planner_.vectorize)] = std::move(sk);
      }
    }
  }

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
  r.columns = columns;
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
  DictTxn dicts(t);
  for (size_t i = 0; i < rows.size(); i++) {
    batch.put(rowKey(t, new_rows[i]), rowPayload(t, new_rows[i]));
    for (const IndexSchema& ix : t.indexes) {
      std::string oi = indexKey(t, ix, rows[i]), ni = indexKey(t, ix, new_rows[i]);
      if (oi != ni) batch.put(ni, "");
    }
  }
  dicts.stage(batch);
  db_.write(batch);
  dicts.commit();
  return message("updated " + plural(rows.size(), "row"));
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

void Engine::analyzeTable(TableSchema& t) {
  size_t ncols = t.cols.size();
  std::vector<HyperLogLog> distinct(ncols);  // exact for small columns, then ~0.8% error
  std::vector<std::vector<Value>> sample(ncols);
  std::vector<uint64_t> nulls(ncols, 0);
  uint64_t rows = 0;
  uint64_t rng = 0x9e3779b97f4a7c15ull;
  // Rows are decoded in place from the store into one reused buffer.
  std::string prefix = tablePrefix(t.id);
  RowCursor cursor(db_, t, prefix, prefixSuccessor(prefix), nullptr, nullptr);
  Row row;
  while (cursor.next(&row)) {
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
      distinct[c].add(row[c].hash());
      if (sample[c].size() < kAnalyzeSample) sample[c].push_back(row[c]);
      else if (slot < kAnalyzeSample) sample[c][slot] = row[c];
    }
  }

  TableStats st;
  st.rows = rows;
  st.analyzed = true;
  for (size_t c = 0; c < ncols; c++) {
    ColumnStats cs;
    cs.ndv = std::min(distinct[c].estimate(), rows - nulls[c]);
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
