#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pkql/ast.h"
#include "pkql/expr.h"
#include "pkql/storage.h"

namespace pkql {

struct AggSpec {
  std::string fn;  // count, sum, avg, min, max, count_distinct
  ExprPtr arg;     // bound to the input schema; nullptr for count()
};

struct SortSpec {
  ExprPtr expr;  // bound to the input schema
  bool desc = false;
};

// ---------------------------------------------------------------------------
// Logical plan: what to compute. Built directly from the pipeline.
// ---------------------------------------------------------------------------

enum class LKind { Scan, Filter, Project, Join, Aggregate, Sort, Limit, Distinct };

struct LNode;
using LPtr = std::shared_ptr<LNode>;

struct LNode {
  LKind kind = LKind::Scan;
  Schema schema;  // output columns
  std::vector<LPtr> kids;

  const TableSchema* table = nullptr;  // Scan
  ExprPtr pred;                        // Filter predicate / Join condition (bound to left ++ right)
  bool left_join = false;
  std::vector<ExprPtr> exprs;  // Project outputs / Aggregate group keys
  std::vector<AggSpec> aggs;
  std::vector<SortSpec> sort;
  int64_t limit = -1;  // -1 = no limit
  int64_t offset = 0;
};

using Lets = std::map<std::string, std::shared_ptr<Pipeline>>;

// Turns a parsed pipeline into a logical plan, binding and type-checking
// every expression along the way.
LPtr buildLogicalPlan(const Pipeline& p, Catalog& catalog, const Lets& lets);

// ---------------------------------------------------------------------------
// Physical plan: how to compute it, with cost estimates.
// ---------------------------------------------------------------------------

enum class PKind {
  TableScan,      // primary-key range (or full table, or a single-key lookup)
  IndexScan,      // secondary-index range, then fetch each row by primary key
  Filter,
  Project,
  HashJoin,
  IndexJoin,      // for each outer row, look up the inner table by key
  NestedLoopJoin,
  HashAggregate,
  Sort,
  TopN,           // sort + limit with a bounded heap
  Limit,
  Distinct,
};

// Statistics the estimator tracks for each output column.
struct ColEstimate {
  double ndv = -1;                  // distinct values; -1 = unknown
  const ColumnStats* base = nullptr;  // histogram, when the column comes straight from a table
  bool unique = false;              // a single-column primary key
};

struct PNode;
using PPtr = std::shared_ptr<PNode>;

struct PNode {
  PKind kind = PKind::TableScan;
  Schema schema;
  std::vector<PPtr> kids;

  // Scans and index joins.
  const TableSchema* table = nullptr;
  const IndexSchema* index = nullptr;  // nullptr = primary key
  std::vector<ExprPtr> eq;  // equality values for the leading key columns
  ExprPtr lo, hi;           // optional range on the next key column
  bool lo_incl = true, hi_incl = true;
  bool point = false;       // eq covers the whole primary key: one get()
  std::vector<bool> needed; // table columns the query reads (column pruning); empty = all

  ExprPtr pred;  // filter predicate, or residual predicate of a scan / join
  bool left_join = false;
  std::vector<ExprPtr> left_keys, right_keys;  // hash join keys
  std::vector<ExprPtr> exprs;                  // project outputs / group keys
  std::vector<AggSpec> aggs;
  std::vector<SortSpec> sort;
  int64_t limit = -1, offset = 0;

  // Estimates.
  double rows = 0;
  double cost = 0;     // total cost to produce every row
  double startup = 0;  // cost before the first row (blocking operators)
  std::vector<int> order;  // output columns the rows arrive sorted by (ascending)
  bool order_needed = false;  // a parent relies on `order` (e.g. a sort it replaced)
  std::vector<ColEstimate> cols;

  // Filled in by `explain analyze`.
  uint64_t actual_rows = 0;
  double actual_ms = 0;
  bool executed = false;
};

struct PlannerOptions {
  bool reorder_joins = true;        // dynamic-programming join ordering
  bool use_indexes = true;          // index scans and index joins
  bool use_interesting_orders = true;
};

PPtr optimize(const LPtr& logical, const PlannerOptions& opts = PlannerOptions());

std::string explainPlan(const PNode& root, bool with_actuals);

}  // namespace pkql
