#pragma once

#include <cmath>
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

// Float sums with Kahan-Babuska-Neumaier compensation (Neumaier, 1974): the
// low-order bits each addition rounds away are collected in `c`, so the
// result hardly depends on the order of the additions (row by row, or
// merged from parallel morsels) and stays accurate over millions of rows.
// SQLite's sum() does the same since 3.43.
struct FloatSum {
  double sum = 0, c = 0;
  FloatSum& operator+=(double x) {
    double t = sum + x;
    c += std::fabs(sum) >= std::fabs(x) ? (sum - t) + x : (x - t) + sum;
    sum = t;
    return *this;
  }
  FloatSum& operator+=(const FloatSum& o) {
    *this += o.sum;
    *this += o.c;
    return *this;
  }
  operator double() const { return sum + c; }
};

// Percentile aggregates (median, p90, p95, p99): the fraction each asks for,
// and the exact value, interpolated like percentile_cont (null if empty).
double quantileOf(const std::string& fn);
Value quantile(std::vector<double> values, double q);

// The encoded key range [lo, hi) a primary-key TableScan node reads.
void tableScanRange(const PNode& scan, std::string* lo, std::string* hi);

// Runs a plan to completion and returns every row.
std::vector<Row> runPlan(PNode& plan, ExecContext& ctx);

}  // namespace pkql
