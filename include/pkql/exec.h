#pragma once

#include <memory>

#include "persistkv/db.h"
#include "pkql/plan.h"

namespace pkql {

// Volcano-style iterator: open() once, then next() until it returns false.
// Each operator pulls rows from its children one at a time, so pipelines
// like `limit` over a scan stop reading as soon as they have enough rows.
class Operator {
 public:
  explicit Operator(PNode& node) : node_(node) {}
  virtual ~Operator() = default;
  virtual void open() = 0;
  virtual bool next(Row* row) = 0;

 protected:
  PNode& node_;
};

struct ExecContext {
  pkv::DB& db;
  bool analyze = false;  // record actual rows and time per operator
};

std::unique_ptr<Operator> buildOperator(PNode& plan, ExecContext& ctx);

// Runs a plan to completion and returns every row.
std::vector<Row> runPlan(PNode& plan, ExecContext& ctx);

}  // namespace pkql
