#include "pkql/exec.h"

#include <algorithm>
#include <chrono>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "pkql/codec.h"
#include "pkql/vector.h"

namespace pkql {

namespace {

using Clock = std::chrono::steady_clock;

// A sorted, batched lookup costs about this many skipped rows (measured:
// ~150-300 ns per key vs ~25-35 ns per skipped row). Keep in sync with the
// optimizer's kBatchedLookup / kSkipRow.
constexpr double kBatchedLookupVsSkip = 6.0;

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
  KeyReader(pkv::DB& db, const PNode& p) : db_(db), p_(p), plan_(*p.table, p.needed.empty() ? nullptr : &p.needed) {}

  // `pred`, if given, is the scan's residual filter: rows that fail it are
  // dropped inside the storage scan without being copied out.
  void start(const std::vector<Value>& eq, const Row& ctx_row, const Expr* pred = nullptr) {
    done_ = false;
    cursor_.reset();
    rows_.reset();
    pending_keys_.clear();
    pending_pos_ = 0;
    batched_ = false;
    batch_pos_ = batch_count_ = 0;
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
    if (!p_.index) {
      rows_ = std::make_unique<RowCursor>(db_, *p_.table, lo, hi, needed(), pred);
      if (p_.reverse) rows_->setReverse();
      return;
    }
    // A sorted fetch returns rows in primary-key order, so it is only used
    // when nothing above relies on the index's order.
    if (p_.kind == PKind::IndexScan && !p_.order_needed) {
      startSortedFetchIfCheaper(std::move(lo), hi);
      return;
    }
    // A join probe with equality on every index column finds row keys that
    // are already in index order, so it can fetch them in batches without
    // changing its output order.
    if (p_.kind == PKind::IndexJoin && eq.size() == p_.index->cols.size()) {
      std::vector<std::string> keys;
      collectRowKeys(db_, *p_.table, *p_.index, std::move(lo), hi, &keys);
      startBatchedFetch(std::move(keys));
      return;
    }
    cursor_ = std::make_unique<Cursor>(db_, lo, hi);
    if (p_.reverse) cursor_->setReverse();
  }

  // Sorted index fetch (like a bitmap heap scan): collect the matching row
  // keys and sort them, then look them up in batches (neighbouring keys share
  // leaf pages) or, when matches are dense, read the table once in key order
  // decoding only those rows.
  void startSortedFetchIfCheaper(std::string index_lo, const std::string& index_hi) {
    std::vector<std::string> keys;
    collectRowKeys(db_, *p_.table, *p_.index, std::move(index_lo), index_hi, &keys);
    sortKeys(keys);
    double table_rows = static_cast<double>(p_.table->stats.rows);
    if (static_cast<double>(keys.size()) * kBatchedLookupVsSkip < table_rows) {
      startBatchedFetch(std::move(keys));
      return;
    }
    std::string prefix = tablePrefix(p_.table->id);
    std::string lo = keys.empty() ? prefix : keys.front();
    std::string hi = keys.empty() ? prefix : prefixSuccessor(keys.back());
    rows_ = std::make_unique<RowCursor>(db_, *p_.table, lo, hi, needed(), pred_);
    rows_->onlyKeys(std::move(keys));
  }

  bool next(Row* row) {
    if (point_pending_) {
      point_pending_ = false;
      if (point_key_.size() > pkv::kMaxKeySize || !fetch(point_key_, row)) return false;
      return !pred_ || isTrue(eval(*pred_, *row));
    }
    if (rows_) return rows_->next(row);
    if (batched_) {
      while (batch_pos_ >= batch_count_) {
        if (pending_pos_ >= pending_keys_.size()) return false;
        fetchBatch();
      }
      std::swap(*row, batch_rows_[batch_pos_++]);  // hand the row over; take the caller's old buffer back
      return true;
    }
    while (pending_pos_ < pending_keys_.size()) {
      const std::string& rk = pending_keys_[pending_pos_++];
      if (!fetch(rk, row)) throw Error("index " + p_.index->name + " points at a missing row");
      if (!pred_ || isTrue(eval(*pred_, *row))) return true;
    }
    if (!cursor_) return false;
    std::string key, value;
    while (cursor_->next(&key, &value)) {
      std::string rk = rowKeyFromIndexEntry(*p_.table, *p_.index, key);
      if (!fetch(rk, row)) throw Error("index " + p_.index->name + " points at a missing row");
      if (!pred_ || isTrue(eval(*pred_, *row))) return true;
    }
    return false;
  }

 private:
  const std::vector<bool>* needed() const { return p_.needed.empty() ? nullptr : &p_.needed; }

  // Looks the row keys (sorted) up a batch at a time.
  void startBatchedFetch(std::vector<std::string> keys) {
    pending_keys_ = std::move(keys);
    pending_pos_ = 0;
    batched_ = true;
    batch_size_ = 16;
  }

  // Looks up the next batch of sorted row keys with one batched storage call
  // and keeps the decoded rows that pass the filter.
  void fetchBatch() {
    size_t n = std::min(batch_size_, pending_keys_.size() - pending_pos_);
    batch_size_ = std::min<size_t>(batch_size_ * 4, 1024);  // small first batch, for `take`
    batch_keys_.assign(std::make_move_iterator(pending_keys_.begin() + static_cast<long>(pending_pos_)),
                       std::make_move_iterator(pending_keys_.begin() + static_cast<long>(pending_pos_ + n)));
    pending_pos_ += n;
    batch_pos_ = batch_count_ = 0;
    size_t found = 0;
    db_.multiGetRaw(batch_keys_, [&](size_t i, const char* v, size_t len) {
      found++;
      if (batch_count_ == batch_rows_.size()) batch_rows_.emplace_back();
      Row& r = batch_rows_[batch_count_];
      decodeStoredRowInto(plan_, batch_keys_[i], std::string_view(v, len), r);
      if (!pred_ || isTrue(eval(*pred_, r))) batch_count_++;
    });
    if (found != n) throw Error("index " + p_.index->name + " points at a missing row");
  }

  // Looks a row up by key and decodes it straight from the cached page.
  bool fetch(const std::string& row_key, Row* row) {
    return db_.getRaw(row_key, [&](const char* v, size_t n) {
      decodeStoredRowInto(plan_, row_key, std::string_view(v, n), *row);
    });
  }

  pkv::DB& db_;
  const PNode& p_;
  DecodePlan plan_;
  std::unique_ptr<Cursor> cursor_;    // secondary-index entries
  std::unique_ptr<RowCursor> rows_;  // primary-key range (or sorted index fetch)
  std::vector<std::string> pending_keys_;  // index matches to look up
  size_t pending_pos_ = 0;
  bool batched_ = false;  // pending_keys_ are sorted and fetched in batches
  size_t batch_size_ = 16;
  std::vector<std::string> batch_keys_;
  std::vector<Row> batch_rows_;
  size_t batch_pos_ = 0, batch_count_ = 0;
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

// Hash join with a flat table: build rows live in one array, and buckets
// and chains are plain integer arrays (no node allocation per row). Keys
// are compared in place by reference, and output rows are written into
// the caller's buffer, reusing its storage.
class HashJoinOp : public Operator {
 public:
  HashJoinOp(PNode& n, std::unique_ptr<Operator> l, std::unique_ptr<Operator> r)
      : Operator(n), left_(std::move(l)), right_(std::move(r)) {}

  void open() override {
    right_->open();
    rows_.clear();
    hashes_.clear();
    Row r;
    while (right_->next(&r)) {
      hashes_.push_back(keyHash(node_.right_keys, r));
      rows_.push_back(std::move(r));
      r = Row();
    }
    size_t buckets = 16;
    while (buckets < rows_.size() * 2) buckets <<= 1;
    mask_ = buckets - 1;
    head_.assign(buckets, -1);
    chain_.assign(rows_.size(), -1);
    for (size_t i = 0; i < rows_.size(); i++) {
      size_t b = hashes_[i] & mask_;
      chain_[i] = head_[b];
      head_[b] = static_cast<int32_t>(i);
    }
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
        hash_ = keyHash(node_.left_keys, cur_);
        pos_ = head_[hash_ & mask_];
      }
      while (pos_ >= 0) {
        int32_t i = pos_;
        pos_ = chain_[i];
        if (hashes_[i] != hash_ || !keysEqual(cur_, rows_[i])) continue;
        row->assign(cur_.begin(), cur_.end());
        row->insert(row->end(), rows_[i].begin(), rows_[i].end());
        if (!node_.pred || isTrue(eval(*node_.pred, *row))) {
          matched_ = true;
          return true;
        }
      }
      have_left_ = false;
      if (node_.left_join && !matched_) {
        row->assign(cur_.begin(), cur_.end());
        row->resize(cur_.size() + right_width_);
        for (size_t c = cur_.size(); c < row->size(); c++) (*row)[c] = Value();
        return true;
      }
    }
  }

 private:
  // Value hashing treats 1 and 1.0 alike, matching compare().
  static uint64_t keyHash(const std::vector<ExprPtr>& exprs, const Row& r) {
    uint64_t h = 1469598103934665603ull;
    Value scratch;
    for (const ExprPtr& e : exprs) h = (h ^ evalRef(*e, r, scratch).hash()) * 1099511628211ull;
    return h;
  }

  bool keysEqual(const Row& left, const Row& right) const {
    Value a, b;
    for (size_t k = 0; k < node_.left_keys.size(); k++) {
      if (compare(evalRef(*node_.left_keys[k], left, a), evalRef(*node_.right_keys[k], right, b)) != 0) return false;
    }
    return true;
  }

  std::unique_ptr<Operator> left_, right_;
  std::vector<Row> rows_;         // build side
  std::vector<uint64_t> hashes_;  // key hash per build row
  std::vector<int32_t> head_;     // bucket -> first build row (-1 = empty)
  std::vector<int32_t> chain_;    // build row -> next row in the same bucket
  size_t mask_ = 0;
  Row cur_;
  uint64_t hash_ = 0;
  int32_t pos_ = -1;
  bool have_left_ = false, matched_ = false;
  size_t right_width_ = 0;
};

class IndexJoinOp : public Operator {
 public:
  IndexJoinOp(PNode& n, std::unique_ptr<Operator> outer, ExecContext& ctx)
      : Operator(n), outer_(std::move(outer)), reader_(ctx.db, n) {
    const std::vector<int>& cols = node_.index ? node_.index->cols : node_.table->pk;
    prefix_.assign(cols.begin(), cols.begin() + static_cast<long>(node_.eq.size()));
  }

  void open() override {
    outer_->open();
    have_outer_ = false;
  }

  bool next(Row* row) override {
    while (true) {
      if (!have_outer_) {
        if (!outer_->next(&cur_)) return false;
        have_outer_ = true;
        matched_ = false;
        reader_.start(evalAll(node_.eq, cur_, node_.table, &prefix_), cur_);
      }
      while (reader_.next(&inner_)) {
        // Written into the caller's buffer, reusing its storage.
        row->assign(cur_.begin(), cur_.end());
        row->insert(row->end(), inner_.begin(), inner_.end());
        if (!node_.pred || isTrue(eval(*node_.pred, *row))) {
          matched_ = true;
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
  std::vector<int> prefix_;  // the key columns the probe's equalities fix
  Row cur_, inner_;
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

enum class AggFn { Count, CountArg, Sum, Avg, Min, Max, CountDistinct, Quantile };

AggFn aggKind(const AggSpec& a) {
  if (a.fn == "count") return a.arg ? AggFn::CountArg : AggFn::Count;
  if (a.fn == "sum") return AggFn::Sum;
  if (a.fn == "avg") return AggFn::Avg;
  if (a.fn == "min") return AggFn::Min;
  if (a.fn == "max") return AggFn::Max;
  if (a.fn == "count_distinct") return AggFn::CountDistinct;
  return AggFn::Quantile;
}

// The fraction a percentile aggregate asks for.
double quantileOf(const std::string& fn) {
  if (fn == "p90") return 0.90;
  if (fn == "p95") return 0.95;
  if (fn == "p99") return 0.99;
  return 0.5;  // median
}

// Exact percentile with linear interpolation between neighbours (like
// percentile_cont in PostgreSQL and SQLite).
Value quantile(std::vector<double> v, double q) {
  if (v.empty()) return Value::null();
  double pos = q * static_cast<double>(v.size() - 1);
  size_t lo = static_cast<size_t>(pos);
  std::nth_element(v.begin(), v.begin() + static_cast<long>(lo), v.end());
  double a = v[lo];
  if (lo + 1 >= v.size()) return Value::real(a);
  double b = *std::min_element(v.begin() + static_cast<long>(lo) + 1, v.end());
  return Value::real(a + (b - a) * (pos - static_cast<double>(lo)));
}

struct AggState {
  int64_t count = 0;
  bool is_float = false;
  int64_t isum = 0;
  double fsum = 0;
  Value min, max;
  bool seen = false;  // any non-null input
  std::unordered_set<Row, RowHash, RowEq> distinct;
  std::vector<double> values;  // percentiles
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
      case AggFn::Quantile:
        s.values.push_back(v.asNumber());
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
    if (a.fn == "max") return s.max;
    return quantile(s.values, quantileOf(a.fn));
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

void tableScanRange(const PNode& p, std::string* lo, std::string* hi) {
  std::vector<int> prefix(p.table->pk.begin(), p.table->pk.begin() + static_cast<long>(p.eq.size()));
  keyRange(tablePrefix(p.table->id), evalAll(p.eq, Row(), p.table, &prefix), p, Row(), lo, hi);
}

bool isIdentityProject(const PNode& p) {
  if (p.exprs.size() != p.kids[0]->schema.size()) return false;
  for (size_t i = 0; i < p.exprs.size(); i++) {
    if (p.exprs[i]->kind != ExprKind::Column || p.exprs[i]->index != static_cast<int>(i)) return false;
  }
  return true;
}

std::unique_ptr<Operator> buildOperator(PNode& p, ExecContext& ctx) {
  auto kid = [&](size_t i) { return buildOperator(*p.kids[i], ctx); };
  std::unique_ptr<Operator> op;
  if (p.vectorized) {
    op = buildVectorizedOperator(p, ctx);
    if (ctx.analyze) op = std::make_unique<Instrumented>(p, std::move(op));
    return op;
  }
  switch (p.kind) {
    case PKind::TableScan:
    case PKind::IndexScan: op = std::make_unique<ScanOp>(p, ctx); break;
    case PKind::Filter: op = std::make_unique<FilterOp>(p, kid(0)); break;
    case PKind::Project:
      // A projection that passes every column through unchanged is skipped
      // (kept under explain analyze so the plan shape matches the output).
      if (!ctx.analyze && isIdentityProject(p)) return kid(0);
      op = std::make_unique<ProjectOp>(p, kid(0));
      break;
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
