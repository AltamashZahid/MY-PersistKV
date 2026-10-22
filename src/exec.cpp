#include "pkql/exec.h"

#include <algorithm>
#include <chrono>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "pkql/codec.h"

namespace pkql {

namespace {

using Clock = std::chrono::steady_clock;

struct RowEq {
  bool operator()(const Row& a, const Row& b) const { return a == b; }
};

Row concat(const Row& a, const Row& b) {
  Row r = a;
  r.insert(r.end(), b.begin(), b.end());
  return r;
}

Row nullRow(size_t n) { return Row(n); }

// Key range [lo, hi) in encoded form for `prefix + eq values + optional
// range on the next key column`.
void keyRange(const std::string& prefix, const std::vector<Value>& eq, const PNode& p, const Row& ctx_row,
              std::string* lo, std::string* hi) {
  std::string base = prefix;
  for (const Value& v : eq) encodeKey(v, base);
  *lo = base;
  *hi = prefixSuccessor(base);
  if (p.lo) {
    std::string b = base;
    encodeKey(eval(*p.lo, ctx_row), b);
    *lo = p.lo_incl ? b : prefixSuccessor(b);
  }
  if (p.hi) {
    std::string b = base;
    encodeKey(eval(*p.hi, ctx_row), b);
    *hi = p.hi_incl ? prefixSuccessor(b) : b;
  }
}

std::vector<Value> evalAll(const std::vector<ExprPtr>& exprs, const Row& row, const TableSchema* t,
                           const std::vector<int>* key_cols) {
  std::vector<Value> out;
  for (size_t i = 0; i < exprs.size(); i++) {
    Value v = eval(*exprs[i], row);
    if (t && key_cols) v = v.castTo(t->cols[(*key_cols)[i]].type);
    out.push_back(std::move(v));
  }
  return out;
}

// Reads rows of one table through the primary key or a secondary index.
class KeyReader {
 public:
  KeyReader(pkv::DB& db, const PNode& p) : db_(db), p_(p) {}

  // `pred`, if given, is the scan's residual filter: rows that fail it are
  // dropped inside the storage scan without being copied out.
  void start(const std::vector<Value>& eq, const Row& ctx_row, const Expr* pred = nullptr) {
    done_ = false;
    cursor_.reset();
    rows_.reset();
    pred_ = pred;
    const std::vector<int>& cols = p_.index ? p_.index->cols : p_.table->pk;
    if (p_.point && !p_.index && eq.size() == cols.size()) {
      point_key_ = tablePrefix(p_.table->id);
      for (const Value& v : eq) encodeKey(v, point_key_);
      point_pending_ = true;
      return;
    }
    point_pending_ = false;
    std::string lo, hi;
    keyRange(p_.index ? indexPrefix(p_.index->id) : tablePrefix(p_.table->id), eq, p_, ctx_row, &lo, &hi);
    if (p_.index) cursor_ = std::make_unique<Cursor>(db_, lo, hi);
    else rows_ = std::make_unique<RowCursor>(db_, *p_.table, lo, hi, needed(), pred);
  }

  bool next(Row* row) {
    if (point_pending_) {
      point_pending_ = false;
      std::string payload;
      if (point_key_.size() > pkv::kMaxKeySize || !db_.get(point_key_, &payload)) return false;
      *row = decodeStoredRow(*p_.table, point_key_, payload, needed());
      return !pred_ || isTrue(eval(*pred_, *row));
    }
    if (rows_) return rows_->next(row);
    if (!cursor_) return false;
    std::string key, value;
    while (cursor_->next(&key, &value)) {
      std::string rk = rowKeyFromIndexEntry(*p_.table, *p_.index, key);
      std::string payload;
      if (!db_.get(rk, &payload)) throw Error("index " + p_.index->name + " points at a missing row");
      *row = decodeStoredRow(*p_.table, rk, payload, needed());
      if (!pred_ || isTrue(eval(*pred_, *row))) return true;
    }
    return false;
  }

 private:
  const std::vector<bool>* needed() const { return p_.needed.empty() ? nullptr : &p_.needed; }

  pkv::DB& db_;
  const PNode& p_;
  std::unique_ptr<Cursor> cursor_;    // secondary-index entries
  std::unique_ptr<RowCursor> rows_;  // primary-key range
  const Expr* pred_ = nullptr;
  std::string point_key_;
  bool point_pending_ = false;
  bool done_ = false;
};

class ScanOp : public Operator {
 public:
  ScanOp(PNode& n, ExecContext& ctx) : Operator(n), reader_(ctx.db, n) {}
  void open() override {
    const std::vector<int>& cols = node_.index ? node_.index->cols : node_.table->pk;
    std::vector<int> prefix(cols.begin(), cols.begin() + static_cast<long>(node_.eq.size()));
    reader_.start(evalAll(node_.eq, Row(), node_.table, &prefix), Row(), node_.pred.get());
  }
  bool next(Row* row) override { return reader_.next(row); }

 private:
  KeyReader reader_;
};

class FilterOp : public Operator {
 public:
  FilterOp(PNode& n, std::unique_ptr<Operator> child) : Operator(n), child_(std::move(child)) {}
  void open() override { child_->open(); }
  bool next(Row* row) override {
    while (child_->next(row)) {
      if (isTrue(eval(*node_.pred, *row))) return true;
    }
    return false;
  }

 private:
  std::unique_ptr<Operator> child_;
};

class ProjectOp : public Operator {
 public:
  ProjectOp(PNode& n, std::unique_ptr<Operator> child) : Operator(n), child_(std::move(child)) {}
  void open() override { child_->open(); }
  bool next(Row* row) override {
    if (!child_->next(&in_)) return false;
    row->clear();
    for (const ExprPtr& e : node_.exprs) row->push_back(eval(*e, in_));
    return true;
  }

 private:
  std::unique_ptr<Operator> child_;
  Row in_;
};

class HashJoinOp : public Operator {
 public:
  HashJoinOp(PNode& n, std::unique_ptr<Operator> l, std::unique_ptr<Operator> r)
      : Operator(n), left_(std::move(l)), right_(std::move(r)) {}

  void open() override {
    right_->open();
    table_.clear();
    Row r;
    while (right_->next(&r)) table_.emplace(keys(node_.right_keys, r), r);
    right_width_ = node_.kids[1]->schema.size();
    left_->open();
    have_left_ = false;
  }

  bool next(Row* row) override {
    while (true) {
      if (!have_left_) {
        if (!left_->next(&cur_)) return false;
        have_left_ = true;
        matched_ = false;
        range_ = table_.equal_range(keys(node_.left_keys, cur_));
      }
      while (range_.first != range_.second) {
        Row out = concat(cur_, range_.first->second);
        ++range_.first;
        if (!node_.pred || isTrue(eval(*node_.pred, out))) {
          matched_ = true;
          *row = std::move(out);
          return true;
        }
      }
      have_left_ = false;
      if (node_.left_join && !matched_) {
        *row = concat(cur_, nullRow(right_width_));
        return true;
      }
    }
  }

 private:
  static Row keys(const std::vector<ExprPtr>& exprs, const Row& r) {
    Row k;
    for (const ExprPtr& e : exprs) k.push_back(eval(*e, r));  // Value hashing treats 1 and 1.0 alike
    return k;
  }

  std::unique_ptr<Operator> left_, right_;
  std::unordered_multimap<Row, Row, RowHash, RowEq> table_;
  using Iter = std::unordered_multimap<Row, Row, RowHash, RowEq>::iterator;
  std::pair<Iter, Iter> range_;
  Row cur_;
  bool have_left_ = false, matched_ = false;
  size_t right_width_ = 0;
};

class IndexJoinOp : public Operator {
 public:
  IndexJoinOp(PNode& n, std::unique_ptr<Operator> outer, ExecContext& ctx)
      : Operator(n), outer_(std::move(outer)), reader_(ctx.db, n) {}

  void open() override {
    outer_->open();
    have_outer_ = false;
  }

  bool next(Row* row) override {
    const std::vector<int>& cols = node_.index ? node_.index->cols : node_.table->pk;
    std::vector<int> prefix(cols.begin(), cols.begin() + static_cast<long>(node_.eq.size()));
    while (true) {
      if (!have_outer_) {
        if (!outer_->next(&cur_)) return false;
        have_outer_ = true;
        matched_ = false;
        reader_.start(evalAll(node_.eq, cur_, node_.table, &prefix), cur_);
      }
      Row inner;
      while (reader_.next(&inner)) {
        Row out = concat(cur_, inner);
        if (!node_.pred || isTrue(eval(*node_.pred, out))) {
          matched_ = true;
          *row = std::move(out);
          return true;
        }
      }
      have_outer_ = false;
      if (node_.left_join && !matched_) {
        *row = concat(cur_, nullRow(node_.table->cols.size()));
        return true;
      }
    }
  }

 private:
  std::unique_ptr<Operator> outer_;
  KeyReader reader_;
  Row cur_;
  bool have_outer_ = false, matched_ = false;
};

class NestedLoopJoinOp : public Operator {
 public:
  NestedLoopJoinOp(PNode& n, std::unique_ptr<Operator> l, std::unique_ptr<Operator> r)
      : Operator(n), left_(std::move(l)), right_(std::move(r)) {}

  void open() override {
    right_->open();
    inner_.clear();
    Row r;
    while (right_->next(&r)) inner_.push_back(r);
    left_->open();
    have_left_ = false;
  }

  bool next(Row* row) override {
    while (true) {
      if (!have_left_) {
        if (!left_->next(&cur_)) return false;
        have_left_ = true;
        matched_ = false;
        pos_ = 0;
      }
      while (pos_ < inner_.size()) {
        Row out = concat(cur_, inner_[pos_++]);
        if (!node_.pred || isTrue(eval(*node_.pred, out))) {
          matched_ = true;
          *row = std::move(out);
          return true;
        }
      }
      have_left_ = false;
      if (node_.left_join && !matched_) {
        *row = concat(cur_, nullRow(node_.kids[1]->schema.size()));
        return true;
      }
    }
  }

 private:
  std::unique_ptr<Operator> left_, right_;
  std::vector<Row> inner_;
  Row cur_;
  size_t pos_ = 0;
  bool have_left_ = false, matched_ = false;
};

enum class AggFn { Count, CountArg, Sum, Avg, Min, Max, CountDistinct };

AggFn aggKind(const AggSpec& a) {
  if (a.fn == "count") return a.arg ? AggFn::CountArg : AggFn::Count;
  if (a.fn == "sum") return AggFn::Sum;
  if (a.fn == "avg") return AggFn::Avg;
  if (a.fn == "min") return AggFn::Min;
  if (a.fn == "max") return AggFn::Max;
  return AggFn::CountDistinct;
}

struct AggState {
  int64_t count = 0;
  bool is_float = false;
  int64_t isum = 0;
  double fsum = 0;
  Value min, max;
  bool seen = false;  // any non-null input
  std::unordered_set<Row, RowHash, RowEq> distinct;
};

class HashAggregateOp : public Operator {
 public:
  HashAggregateOp(PNode& n, std::unique_ptr<Operator> child) : Operator(n), child_(std::move(child)) {}

  void open() override {
    child_->open();
    groups_.clear();
    order_.clear();
    kinds_.clear();
    for (const AggSpec& a : node_.aggs) kinds_.push_back(aggKind(a));
    Row r, key;
    if (node_.exprs.empty()) {  // `aggregate`: a single group, no hash table needed
      std::vector<AggState>& st = groups_[Row()];
      st.resize(node_.aggs.size());
      order_.push_back(Row());
      while (child_->next(&r)) {
        for (size_t i = 0; i < node_.aggs.size(); i++) update(kinds_[i], node_.aggs[i], st[i], r);
      }
      pos_ = 0;
      return;
    }
    while (child_->next(&r)) {
      key.clear();
      for (const ExprPtr& e : node_.exprs) key.push_back(eval(*e, r));
      auto it = groups_.find(key);
      if (it == groups_.end()) {
        it = groups_.emplace(key, std::vector<AggState>(node_.aggs.size())).first;
        order_.push_back(key);
      }
      for (size_t i = 0; i < node_.aggs.size(); i++) update(kinds_[i], node_.aggs[i], it->second[i], r);
    }
    // `aggregate` (no keys) always yields one row, even over zero rows.
    if (node_.exprs.empty() && groups_.empty()) {
      groups_.emplace(Row(), std::vector<AggState>(node_.aggs.size()));
      order_.push_back(Row());
    }
    pos_ = 0;
  }

  bool next(Row* row) override {
    if (pos_ >= order_.size()) return false;
    const Row& key = order_[pos_++];
    *row = key;
    const std::vector<AggState>& st = groups_[key];
    for (size_t i = 0; i < node_.aggs.size(); i++) row->push_back(result(node_.aggs[i], st[i]));
    return true;
  }

 private:
  static void update(AggFn kind, const AggSpec& a, AggState& s, const Row& r) {
    if (kind == AggFn::Count) {
      s.count++;
      return;
    }
    Value scratch;
    const Value& v = evalRef(*a.arg, r, scratch);
    if (v.isNull()) return;  // aggregates skip nulls (count() counts rows)
    s.count++;
    s.seen = true;
    switch (kind) {
      case AggFn::Sum:
      case AggFn::Avg:
        if (v.type() == Type::Float) s.is_float = true;
        if (v.type() == Type::Int) s.isum += v.asInt();
        s.fsum += v.asNumber();
        break;
      case AggFn::Min:
        if (s.min.isNull() || compare(v, s.min) < 0) s.min = v;
        break;
      case AggFn::Max:
        if (s.max.isNull() || compare(v, s.max) > 0) s.max = v;
        break;
      case AggFn::CountDistinct:
        s.distinct.insert(Row{v});
        break;
      default:
        break;
    }
  }

  static Value result(const AggSpec& a, const AggState& s) {
    if (a.fn == "count") return Value::integer(s.count);
    if (a.fn == "count_distinct") return Value::integer(static_cast<int64_t>(s.distinct.size()));
    if (!s.seen) return Value::null();
    if (a.fn == "sum") return s.is_float || a.arg->type == Type::Float ? Value::real(s.fsum) : Value::integer(s.isum);
    if (a.fn == "avg") return Value::real(s.fsum / static_cast<double>(s.count));
    if (a.fn == "min") return s.min;
    return s.max;
  }

  std::unique_ptr<Operator> child_;
  std::unordered_map<Row, std::vector<AggState>, RowHash, RowEq> groups_;
  std::vector<Row> order_;  // groups in first-seen order
  std::vector<AggFn> kinds_;
  size_t pos_ = 0;
};

// Compares rows by precomputed sort keys.
struct SortRow {
  Row keys;
  Row row;
};

bool lessBy(const std::vector<SortSpec>& spec, const SortRow& a, const SortRow& b) {
  for (size_t i = 0; i < spec.size(); i++) {
    int c = compare(a.keys[i], b.keys[i]);
    if (c != 0) return spec[i].desc ? c > 0 : c < 0;
  }
  return false;
}

SortRow makeSortRow(const std::vector<SortSpec>& spec, Row row) {
  SortRow s;
  for (const SortSpec& k : spec) s.keys.push_back(eval(*k.expr, row));
  s.row = std::move(row);
  return s;
}

class SortOp : public Operator {
 public:
  SortOp(PNode& n, std::unique_ptr<Operator> child) : Operator(n), child_(std::move(child)) {}

  void open() override {
    child_->open();
    rows_.clear();
    Row r;
    if (node_.kind == PKind::TopN) {
      // Keep the best k rows in a max-heap ordered by the sort keys.
      size_t k = static_cast<size_t>(std::max<int64_t>(node_.limit, 0));
      auto cmp = [this](const SortRow& a, const SortRow& b) { return lessBy(node_.sort, a, b); };
      std::priority_queue<SortRow, std::vector<SortRow>, decltype(cmp)> heap(cmp);
      SortRow probe;
      while (k > 0 && child_->next(&r)) {
        probe.keys.clear();
        for (const SortSpec& spec : node_.sort) probe.keys.push_back(eval(*spec.expr, r));
        if (heap.size() < k) {
          probe.row = r;
          heap.push(probe);
        } else if (lessBy(node_.sort, probe, heap.top())) {  // only winners copy their row
          probe.row = r;
          heap.pop();
          heap.push(probe);
        }
      }
      while (!heap.empty()) {
        rows_.push_back(heap.top());
        heap.pop();
      }
    } else {
      while (child_->next(&r)) rows_.push_back(makeSortRow(node_.sort, r));
    }
    std::stable_sort(rows_.begin(), rows_.end(),
                     [this](const SortRow& a, const SortRow& b) { return lessBy(node_.sort, a, b); });
    pos_ = 0;
  }

  bool next(Row* row) override {
    if (pos_ >= rows_.size()) return false;
    *row = std::move(rows_[pos_++].row);
    return true;
  }

 private:
  std::unique_ptr<Operator> child_;
  std::vector<SortRow> rows_;
  size_t pos_ = 0;
};

class LimitOp : public Operator {
 public:
  LimitOp(PNode& n, std::unique_ptr<Operator> child) : Operator(n), child_(std::move(child)) {}
  void open() override {
    child_->open();
    emitted_ = 0;
    skipped_ = 0;
  }
  bool next(Row* row) override {
    if (node_.limit >= 0 && emitted_ >= node_.limit) return false;  // stop pulling from the child
    while (skipped_ < node_.offset) {
      if (!child_->next(row)) return false;
      skipped_++;
    }
    if (!child_->next(row)) return false;
    emitted_++;
    return true;
  }

 private:
  std::unique_ptr<Operator> child_;
  int64_t emitted_ = 0, skipped_ = 0;
};

class DistinctOp : public Operator {
 public:
  DistinctOp(PNode& n, std::unique_ptr<Operator> child) : Operator(n), child_(std::move(child)) {}
  void open() override {
    child_->open();
    seen_.clear();
  }
  bool next(Row* row) override {
    while (child_->next(row)) {
      if (seen_.insert(*row).second) return true;
    }
    return false;
  }

 private:
  std::unique_ptr<Operator> child_;
  std::unordered_set<Row, RowHash, RowEq> seen_;
};

// Wraps an operator to record its row count and inclusive time.
class Instrumented : public Operator {
 public:
  Instrumented(PNode& n, std::unique_ptr<Operator> inner) : Operator(n), inner_(std::move(inner)) {}
  void open() override {
    auto t0 = Clock::now();
    inner_->open();
    node_.actual_ms += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    node_.executed = true;
  }
  bool next(Row* row) override {
    auto t0 = Clock::now();
    bool ok = inner_->next(row);
    node_.actual_ms += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (ok) node_.actual_rows++;
    return ok;
  }

 private:
  std::unique_ptr<Operator> inner_;
};

}  // namespace

std::unique_ptr<Operator> buildOperator(PNode& p, ExecContext& ctx) {
  auto kid = [&](size_t i) { return buildOperator(*p.kids[i], ctx); };
  std::unique_ptr<Operator> op;
  switch (p.kind) {
    case PKind::TableScan:
    case PKind::IndexScan: op = std::make_unique<ScanOp>(p, ctx); break;
    case PKind::Filter: op = std::make_unique<FilterOp>(p, kid(0)); break;
    case PKind::Project: op = std::make_unique<ProjectOp>(p, kid(0)); break;
    case PKind::HashJoin: op = std::make_unique<HashJoinOp>(p, kid(0), kid(1)); break;
    case PKind::IndexJoin: op = std::make_unique<IndexJoinOp>(p, kid(0), ctx); break;
    case PKind::NestedLoopJoin: op = std::make_unique<NestedLoopJoinOp>(p, kid(0), kid(1)); break;
    case PKind::HashAggregate: op = std::make_unique<HashAggregateOp>(p, kid(0)); break;
    case PKind::Sort:
    case PKind::TopN: op = std::make_unique<SortOp>(p, kid(0)); break;
    case PKind::Limit: op = std::make_unique<LimitOp>(p, kid(0)); break;
    case PKind::Distinct: op = std::make_unique<DistinctOp>(p, kid(0)); break;
  }
  if (ctx.analyze) op = std::make_unique<Instrumented>(p, std::move(op));
  return op;
}

std::vector<Row> runPlan(PNode& plan, ExecContext& ctx) {
  auto op = buildOperator(plan, ctx);
  op->open();
  std::vector<Row> rows;
  Row r;
  while (op->next(&r)) rows.push_back(r);
  return rows;
}

}  // namespace pkql
