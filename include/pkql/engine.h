#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "persistkv/db.h"
#include "pkql/ast.h"
#include "pkql/plan.h"
#include "pkql/storage.h"

namespace pkql {

struct Result {
  Schema columns;
  std::vector<Row> rows;
  std::string message;  // for statements that do not return rows, and for explain
  bool has_rows = false;

  std::string format() const;  // an ASCII table (or the message)
};

// A PKQL database: tables stored in a PersistKV key-value store.
// Every statement is atomic (one WriteBatch); statements are serialized.
class Engine {
 public:
  explicit Engine(const std::string& dir, const pkv::Options& options = pkv::Options());

  std::vector<Result> run(std::string_view script);  // every statement in order
  Result execute(const Statement& s);

  // Bulk load: full rows in column order, committed in chunks.
  void insertRows(const std::string& table, const std::vector<Row>& rows);

  // Verifies that every table's row count and secondary indexes match its
  // rows. Returns an empty string if consistent.
  std::string checkIntegrity();

  PlannerOptions& plannerOptions() { return planner_; }

  // Plan cache for queries: plans are reused across literal values of
  // `col == x` / `col in [...]` predicates.
  void setPlanCache(bool on) {
    plan_cache_on_ = on;
    plan_cache_.clear();
  }
  uint64_t planCacheHits() const { return cache_hits_; }
  uint64_t planCacheMisses() const { return cache_misses_; }
  Catalog& catalog() { return catalog_; }
  pkv::DB& db() { return db_; }

 private:
  Result createTable(const Statement& s);
  Result createIndex(const Statement& s);
  Result insert(const Statement& s);
  Result query(const Statement& s);
  Result modify(const Statement& s, const Pipeline& p);
  Result analyze(const Statement& s);
  Result dropTable(const Statement& s);
  Result dropIndex(const Statement& s);
  Result showTables();
  Result describe(const Statement& s);

  TableSchema& table(const std::string& name);
  void stageInsert(TableSchema& t, const std::vector<Row>& rows, pkv::WriteBatch& batch);
  Row checkRow(const TableSchema& t, Row row);
  void analyzeTable(TableSchema& t);

  struct CachedPlan {
    PPtr plan;
    Schema columns;
    uint64_t version = 0;
    std::vector<std::pair<const TableSchema*, uint64_t>> rows_when_planned;
  };
  bool cacheValid(const CachedPlan& c) const;

  pkv::DB db_;
  Catalog catalog_;
  std::unordered_map<std::string, CachedPlan> plan_cache_;
  bool plan_cache_on_ = true;
  uint64_t catalog_version_ = 0;  // bumped by DDL, analyze and let: cached plans go stale
  uint64_t cache_hits_ = 0, cache_misses_ = 0;
  Lets lets_;
  PlannerOptions planner_;
  std::mutex mu_;
};

}  // namespace pkql
