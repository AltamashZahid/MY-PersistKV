#include "pkql/vector.h"

#include <algorithm>
#include <chrono>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "pkql/codec.h"
#include "pkql/storage.h"

namespace pkql {

// ---------------------------------------------------------------------------
// Column vectors
// ---------------------------------------------------------------------------

void ColumnVector::reserve(Type t, size_t n) {
  type = t;
  nulls.assign(n, 1);
  if (t == Type::Int || t == Type::Bool) ints.assign(n, 0);
  if (t == Type::Float) floats.assign(n, 0);
  if (t == Type::Text) {
    texts.resize(n);
    codes.assign(n, -1);
  }
}

Value ColumnVector::get(size_t i) const {
  if (nulls[i]) return Value::null();
  switch (type) {
    case Type::Int: return Value::integer(ints[i]);
    case Type::Bool: return Value::boolean(ints[i] != 0);
    case Type::Float: return Value::real(floats[i]);
    case Type::Text: return Value::text(std::string(textAt(i)));
    default: return Value::null();
  }
}

namespace {

// ---------------------------------------------------------------------------
// Filters: `column op constant` and `column in [constants]` run as typed
// loops that compact a selection vector; anything else is evaluated row by
// row on the rows that are still selected.
// ---------------------------------------------------------------------------

struct Kernel {
  int col = -1;
  Op op = Op::Eq;
  Value lit;                // comparison constant
  std::vector<Value> list;  // `in` constants
  bool in_list = false;
  // Text == / !=: the constant's code in the column's dictionary, looked up
  // once (the early filter calls the kernel row by row).
  mutable const TextDict* code_dict = nullptr;
  mutable int32_t lit_code = -1;
};

Op flipOp(Op op) {
  switch (op) {
    case Op::Lt: return Op::Gt;
    case Op::Le: return Op::Ge;
    case Op::Gt: return Op::Lt;
    case Op::Ge: return Op::Le;
    default: return op;
  }
}

bool isCmp(Op op) { return op == Op::Eq || op == Op::Ne || op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge; }

bool compileKernel(const Expr& e, Kernel* k) {
  if (e.kind == ExprKind::Binary && isCmp(e.op)) {
    const Expr& l = *e.args[0];
    const Expr& r = *e.args[1];
    if (l.kind == ExprKind::Column && r.kind == ExprKind::Literal && !r.value.isNull()) {
      *k = {l.index, e.op, r.value, {}, false};
      return true;
    }
    if (r.kind == ExprKind::Column && l.kind == ExprKind::Literal && !l.value.isNull()) {
      *k = {r.index, flipOp(e.op), l.value, {}, false};
      return true;
    }
    return false;
  }
  if (e.kind == ExprKind::InList && e.args[0]->kind == ExprKind::Column) {
    Kernel out;
    out.col = e.args[0]->index;
    out.in_list = true;
    for (size_t i = 1; i < e.args.size(); i++) {
      if (e.args[i]->kind != ExprKind::Literal || e.args[i]->value.isNull()) return false;
      out.list.push_back(e.args[i]->value);
    }
    *k = std::move(out);
    return true;
  }
  return false;
}

template <typename T>
bool cmpHolds(Op op, const T& a, const T& b) {
  switch (op) {
    case Op::Eq: return a == b;
    case Op::Ne: return a != b;
    case Op::Lt: return a < b;
    case Op::Le: return a <= b;
    case Op::Gt: return a > b;
    case Op::Ge: return a >= b;
    default: return false;
  }
}

// Under compare(), null sorts below every non-null constant.
bool nullHolds(Op op) { return op == Op::Lt || op == Op::Le || op == Op::Ne; }

// Keeps the selected rows that pass the kernel; returns the new count.
size_t applyKernel(const Kernel& k, const ColumnVector& col, uint32_t* sel, size_t n) {
  size_t out = 0;
  if (k.in_list) {
    for (size_t j = 0; j < n; j++) {
      uint32_t i = sel[j];
      if (col.nulls[i]) continue;
      Value v = col.get(i);
      for (const Value& c : k.list) {
        if (compare(v, c) == 0) {
          sel[out++] = i;
          break;
        }
      }
    }
    return out;
  }
  bool null_keep = nullHolds(k.op);
  switch (col.type) {
    case Type::Int:
    case Type::Bool: {
      if (k.lit.type() == Type::Float) {
        double c = k.lit.asFloat();
        for (size_t j = 0; j < n; j++) {
          uint32_t i = sel[j];
          if (col.nulls[i] ? null_keep : cmpHolds(k.op, static_cast<double>(col.ints[i]), c)) sel[out++] = i;
        }
      } else {
        int64_t c = k.lit.type() == Type::Bool ? (k.lit.asBool() ? 1 : 0) : k.lit.asInt();
        for (size_t j = 0; j < n; j++) {
          uint32_t i = sel[j];
          if (col.nulls[i] ? null_keep : cmpHolds(k.op, col.ints[i], c)) sel[out++] = i;
        }
      }
      return out;
    }
    case Type::Float: {
      double c = k.lit.asNumber();
      for (size_t j = 0; j < n; j++) {
        uint32_t i = sel[j];
        if (col.nulls[i] ? null_keep : cmpHolds(k.op, col.floats[i], c)) sel[out++] = i;
      }
      return out;
    }
    case Type::Text: {
      const std::string& c = k.lit.asText();
      if (col.dict && (k.op == Op::Eq || k.op == Op::Ne)) {
        // Compare codes, not strings: the constant's code is looked up once
        // (-1 if absent, then no coded row can equal it).
        if (k.code_dict != col.dict) {
          k.code_dict = col.dict;
          k.lit_code = col.dict->find(c);
        }
        int32_t want = k.lit_code;
        bool eq = k.op == Op::Eq;
        for (size_t j = 0; j < n; j++) {
          uint32_t i = sel[j];
          bool same;
          if (col.nulls[i]) {
            if (null_keep) sel[out++] = i;
            continue;
          }
          int32_t code = col.codes[i];
          same = code >= 0 ? code == want : col.texts[i] == c;
          if (same == eq) sel[out++] = i;
        }
        return out;
      }
      for (size_t j = 0; j < n; j++) {
        uint32_t i = sel[j];
        if (col.nulls[i] ? null_keep : cmpHolds(k.op, col.textAt(i).compare(c), 0)) sel[out++] = i;
      }
      return out;
    }
    default:
      return 0;
  }
}

// ---------------------------------------------------------------------------
// Batch scan: up to kBatchSize rows per call, decoded straight into columns
// inside PersistKV's zero-copy scan callback. Given the filter's kernels, it
// checks each row as soon as a filtered column is decoded and drops failing
// rows before decoding the rest of them (or copying them into the batch).
// ---------------------------------------------------------------------------

class BatchScanner {
 public:
  BatchScanner(pkv::DB& db, const TableSchema& t, std::string lo, std::string hi, const std::vector<bool>* needed)
      : db_(db), plan_(t, needed), next_(std::move(lo)), hi_(std::move(hi)) {
    cols.resize(t.cols.size());
    for (size_t c = 0; c < t.cols.size(); c++) {
      if (!needed || (*needed)[c]) cols[c].reserve(t.cols[c].type, kBatchSize);
      cols[c].dict = t.dict(static_cast<int>(c));
    }
  }

  // Rows failing any of these kernels are dropped while decoding.
  void filterEarly(const std::vector<Kernel>& kernels) {
    by_col_.assign(cols.size(), {});
    for (const Kernel& k : kernels) by_col_[k.col].push_back(&k);
  }

  // Decodes the next batch; returns its row count (0 = scan finished).
  size_t next() {
    while (!done_) {
      size_t n = scanOnce();
      if (n > 0) return n;  // 0 rows from one call may just mean the filter dropped them all
    }
    return 0;
  }

  std::vector<ColumnVector> cols;  // indexed by table column; unneeded columns stay empty

 private:
  size_t scanOnce() {
    size_t n = 0, scanned = 0;
    std::string last;
    bool full = false;
    auto passes = [&](int c) {
      if (by_col_.empty()) return true;
      for (const Kernel* k : by_col_[c]) {
        uint32_t one = static_cast<uint32_t>(n);
        if (applyKernel(*k, cols[c], &one, 1) == 0) return false;
      }
      return true;
    };
    db_.scanRaw(next_, "", [&](const char* k, size_t kl, const char* v, size_t vl) {
      if (!hi_.empty() && hi_.compare(0, hi_.size(), k, kl) <= 0) {
        done_ = true;
        return false;
      }
      // A selective filter could make one call scan far; bound the work done
      // while PersistKV's tree lock is held.
      if (++scanned > 16 * kBatchSize) {
        resume_at_.assign(k, kl);  // not looked at yet: the next call starts here
        return false;
      }
      std::string_view key(k, kl), payload(v, vl);
      key.remove_prefix(keyPrefixLength(key));
      for (int c : plan_.key_steps) {
        decodeKeyToColumn(key, c >= 0 ? &cols[c] : nullptr, n);
        if (c >= 0 && !passes(c)) return true;
      }
      for (int c : plan_.payload_steps) {
        if (payload.empty()) throw Error("corrupt row");
        decodeValueToColumn(payload, c >= 0 ? &cols[c] : nullptr, n);
        if (c >= 0 && !passes(c)) return true;
      }
      if (++n == kBatchSize) {
        last.assign(k, kl);
        full = true;
        return false;
      }
      return true;
    });
    if (!resume_at_.empty()) {
      next_ = std::move(resume_at_);
      resume_at_.clear();
    } else if (full) {
      next_ = last + '\0';
    } else {
      done_ = true;
    }
    return n;
  }

  pkv::DB& db_;
  DecodePlan plan_;
  std::string next_, hi_;
  std::string resume_at_;  // set when a call stopped at its scan bound
  bool done_ = false;
  std::vector<std::vector<const Kernel*>> by_col_;  // early-filter kernels per column
};

// Filter state for one scan: kernels plus the conjuncts that need row-wise
// evaluation.
class BatchFilter {
 public:
  BatchFilter(const ExprPtr& pred, size_t ncols) : ncols_(ncols) {
    std::vector<ExprPtr> conjs;
    splitConjuncts(pred, conjs);
    for (const ExprPtr& c : conjs) {
      Kernel k;
      if (compileKernel(*c, &k)) kernels_.push_back(std::move(k));
      else generic_.push_back(c);
    }
  }

  // Lets the scanner apply the kernels while decoding; apply() then only
  // evaluates the conjuncts that are not kernels.
  void pushKernelsInto(BatchScanner& scanner) {
    scanner.filterEarly(kernels_);
    kernels_in_scan_ = true;
  }

  // Fills sel with the rows of the batch that pass; returns how many.
  size_t apply(const std::vector<ColumnVector>& cols, size_t n, std::vector<uint32_t>& sel) {
    sel.resize(n);
    for (size_t i = 0; i < n; i++) sel[i] = static_cast<uint32_t>(i);
    size_t m = n;
    if (!kernels_in_scan_) {
      for (const Kernel& k : kernels_) m = applyKernel(k, cols[k.col], sel.data(), m);
    }
    if (!generic_.empty() && m > 0) {
      size_t out = 0;
      for (size_t j = 0; j < m; j++) {
        materialize(cols, sel[j], row_);
        bool pass = true;
        for (const ExprPtr& g : generic_) {
          if (!isTrue(eval(*g, row_))) {
            pass = false;
            break;
          }
        }
        if (pass) sel[out++] = sel[j];
      }
      m = out;
    }
    return m;
  }

  // Row i of the batch as a Row (columns that were not decoded are null).
  void materialize(const std::vector<ColumnVector>& cols, uint32_t i, Row& row) const {
    row.resize(ncols_);
    for (size_t c = 0; c < ncols_; c++) row[c] = cols[c].nulls.empty() ? Value() : cols[c].get(i);
  }

 private:
  size_t ncols_;
  std::vector<Kernel> kernels_;
  bool kernels_in_scan_ = false;
  std::vector<ExprPtr> generic_;
  Row row_;
};

// ---------------------------------------------------------------------------
// Vectorized aggregate: no grouping key, one Int / Text key, or a time
// bucket of an Int column (bucket(ts, 1h)).
// ---------------------------------------------------------------------------

enum class VFn { Count, CountCol, Sum, Avg, Min, Max, CountDistinct, Quantile };

struct VAgg {
  VFn fn;
  int col = -1;
  Type type = Type::Null;
  double q = 0.5;  // Quantile
};

struct VState {
  int64_t count = 0;  // rows (count()) or non-null inputs
  int64_t isum = 0;
  double fsum = 0;
  bool seen = false;  // any non-null input
  int64_t imin = 0, imax = 0;
  double fmin = 0, fmax = 0;
  std::string smin, smax;
  std::unordered_set<int64_t> idistinct;  // count_distinct
  std::unordered_set<std::string> sdistinct;
  std::vector<double> values;  // percentiles
};

void updateState(const VAgg& a, VState& s, const std::vector<ColumnVector>& cols, uint32_t i) {
  if (a.fn == VFn::Count) {
    s.count++;
    return;
  }
  const ColumnVector& c = cols[a.col];
  if (c.nulls[i]) return;
  s.count++;
  bool first = !s.seen;
  s.seen = true;
  switch (a.fn) {
    case VFn::Sum:
    case VFn::Avg:
      if (c.type == Type::Int) {
        s.isum += c.ints[i];
        s.fsum += static_cast<double>(c.ints[i]);
      } else {
        s.fsum += c.floats[i];
      }
      break;
    case VFn::Min:
      if (c.type == Type::Text) {
        if (first || c.textAt(i) < s.smin) s.smin = c.textAt(i);
      } else if (c.type == Type::Float) {
        if (first || c.floats[i] < s.fmin) s.fmin = c.floats[i];
      } else if (first || c.ints[i] < s.imin) {
        s.imin = c.ints[i];
      }
      break;
    case VFn::Max:
      if (c.type == Type::Text) {
        if (first || c.textAt(i) > s.smax) s.smax = c.textAt(i);
      } else if (c.type == Type::Float) {
        if (first || c.floats[i] > s.fmax) s.fmax = c.floats[i];
      } else if (first || c.ints[i] > s.imax) {
        s.imax = c.ints[i];
      }
      break;
    case VFn::CountDistinct:
      if (c.type == Type::Text) s.sdistinct.emplace(c.textAt(i));
      else s.idistinct.insert(c.ints[i]);
      break;
    case VFn::Quantile:
      s.values.push_back(c.type == Type::Float ? c.floats[i] : static_cast<double>(c.ints[i]));
      break;
    default:
      break;
  }
}

// The same result values the row-at-a-time HashAggregate produces.
Value finalValue(const VAgg& a, const VState& s) {
  if (a.fn == VFn::Count || a.fn == VFn::CountCol) return Value::integer(s.count);
  if (a.fn == VFn::CountDistinct) return Value::integer(static_cast<int64_t>(s.idistinct.size() + s.sdistinct.size()));
  if (a.fn == VFn::Quantile) return quantile(s.values, a.q);
  if (!s.seen) return Value::null();
  switch (a.fn) {
    case VFn::Sum: return a.type == Type::Float ? Value::real(s.fsum) : Value::integer(s.isum);
    case VFn::Avg: return Value::real(s.fsum / static_cast<double>(s.count));
    case VFn::Min:
      return a.type == Type::Text ? Value::text(s.smin) : a.type == Type::Float ? Value::real(s.fmin) : Value::integer(s.imin);
    case VFn::Max:
      return a.type == Type::Text ? Value::text(s.smax) : a.type == Type::Float ? Value::real(s.fmax) : Value::integer(s.imax);
    default: return Value::null();
  }
}

void tableRange(const PNode& scan, std::string* lo, std::string* hi) { tableScanRange(scan, lo, hi); }

class VecAggregateOp : public Operator {
 public:
  VecAggregateOp(PNode& n, ExecContext& ctx) : Operator(n), ctx_(ctx) {
    for (const AggSpec& a : n.aggs) {
      VAgg v;
      if (a.fn == "count") v.fn = a.arg ? VFn::CountCol : VFn::Count;
      else if (a.fn == "sum") v.fn = VFn::Sum;
      else if (a.fn == "avg") v.fn = VFn::Avg;
      else if (a.fn == "min") v.fn = VFn::Min;
      else if (a.fn == "max") v.fn = VFn::Max;
      else if (a.fn == "count_distinct") v.fn = VFn::CountDistinct;
      else {
        v.fn = VFn::Quantile;
        v.q = quantileOf(a.fn);
      }
      if (a.arg) {
        v.col = a.arg->index;
        v.type = a.arg->type;
      }
      aggs_.push_back(v);
    }
  }

  void open() override {
    PNode& scan = *node_.kids[0];
    std::string lo, hi;
    tableRange(scan, &lo, &hi);
    BatchScanner scanner(ctx_.db, *scan.table, lo, hi, scan.needed.empty() ? nullptr : &scan.needed);
    BatchFilter filter(scan.pred, scan.table->cols.size());
    filter.pushKernelsInto(scanner);
    std::vector<uint32_t> sel;
    groups_.clear();
    keys_.clear();
    // The key column, and for bucket(col, w) the width keys round down to.
    int key_col = -1;
    int64_t width = 0;      // > 0: round keys down to multiples of it
    bool null_key = false;  // bucket() with a width <= 0 is null for every row
    if (!node_.exprs.empty()) {
      const Expr& k = *node_.exprs[0];
      if (k.kind == ExprKind::Call) {
        key_col = k.args[0]->index;
        width = k.args[1]->value.asInt();
        null_key = width <= 0;
      } else {
        key_col = k.index;
      }
    }
    if (key_col < 0) {  // `aggregate`: one group, even over zero rows
      groups_.emplace_back(aggs_.size());
      keys_.emplace_back();
    }
    std::unordered_map<int64_t, uint32_t> int_groups;
    std::unordered_map<std::string, uint32_t> text_groups;
    std::vector<int64_t> code_groups;  // dictionary code -> group, -1 = none yet
    int64_t null_group = -1;
    uint64_t passed = 0;

    while (size_t n = scanner.next()) {
      size_t m = filter.apply(scanner.cols, n, sel);
      passed += m;
      if (key_col < 0) {
        std::vector<VState>& st = groups_[0];
        for (size_t a = 0; a < aggs_.size(); a++) {
          for (size_t j = 0; j < m; j++) updateState(aggs_[a], st[a], scanner.cols, sel[j]);
        }
        continue;
      }
      const ColumnVector& kc = scanner.cols[key_col];
      for (size_t j = 0; j < m; j++) {
        uint32_t i = sel[j];
        uint32_t g;
        if (kc.nulls[i] || null_key) {
          if (null_group < 0) null_group = newGroup(Value::null());
          g = static_cast<uint32_t>(null_group);
        } else if (width > 0) {
          int64_t x = kc.ints[i], b = x / width;
          if (x % width != 0 && x < 0) b--;
          b *= width;
          auto it = int_groups.find(b);
          if (it == int_groups.end()) it = int_groups.emplace(b, newGroup(Value::integer(b))).first;
          g = it->second;
        } else if (kc.type == Type::Text) {
          // Dictionary-coded keys index an array by code; other text hashes.
          int32_t code = kc.codeAt(i);
          if (code >= 0) {
            if (static_cast<size_t>(code) >= code_groups.size()) code_groups.resize(static_cast<size_t>(code) + 1, -1);
            if (code_groups[code] < 0) code_groups[code] = static_cast<int64_t>(newGroup(kc.get(i)));
            g = static_cast<uint32_t>(code_groups[code]);
          } else {
            std::string key(kc.textAt(i));
            auto it = text_groups.find(key);
            if (it == text_groups.end()) it = text_groups.emplace(key, newGroup(kc.get(i))).first;
            g = it->second;
          }
        } else {
          auto it = int_groups.find(kc.ints[i]);
          if (it == int_groups.end()) it = int_groups.emplace(kc.ints[i], newGroup(kc.get(i))).first;
          g = it->second;
        }
        for (size_t a = 0; a < aggs_.size(); a++) updateState(aggs_[a], groups_[g][a], scanner.cols, i);
      }
    }
    scan.actual_rows = passed;
    scan.executed = true;
    pos_ = 0;
  }

  bool next(Row* row) override {
    if (pos_ >= groups_.size()) return false;
    row->clear();
    if (!node_.exprs.empty()) row->push_back(keys_[pos_]);
    for (size_t a = 0; a < aggs_.size(); a++) row->push_back(finalValue(aggs_[a], groups_[pos_][a]));
    pos_++;
    return true;
  }

 private:
  uint32_t newGroup(Value key) {
    groups_.emplace_back(aggs_.size());
    keys_.push_back(std::move(key));
    return static_cast<uint32_t>(groups_.size() - 1);
  }

  ExecContext& ctx_;
  std::vector<VAgg> aggs_;
  std::vector<std::vector<VState>> groups_;  // in first-seen order
  std::vector<Value> keys_;
  size_t pos_ = 0;
};

// ---------------------------------------------------------------------------
// Vectorized top-N: sort keys are compared in the column arrays, and only
// rows that make it into the heap are turned into Rows.
// ---------------------------------------------------------------------------

class VecTopNOp : public Operator {
 public:
  VecTopNOp(PNode& n, ExecContext& ctx) : Operator(n), ctx_(ctx) {}

  void open() override {
    PNode& scan = *node_.kids[0];
    std::string lo, hi;
    tableRange(scan, &lo, &hi);
    BatchScanner scanner(ctx_.db, *scan.table, lo, hi, scan.needed.empty() ? nullptr : &scan.needed);
    BatchFilter filter(scan.pred, scan.table->cols.size());
    filter.pushKernelsInto(scanner);
    std::vector<uint32_t> sel;
    size_t k = static_cast<size_t>(std::max<int64_t>(node_.limit, 0));
    heap_.clear();
    uint64_t passed = 0;
    Entry probe;

    while (size_t n = scanner.next()) {
      size_t m = filter.apply(scanner.cols, n, sel);
      passed += m;
      for (size_t j = 0; j < m && k > 0; j++) {
        uint32_t i = sel[j];
        if (heap_.size() == k && !beatsWorst(scanner.cols, i)) continue;  // cheap reject, no Row built
        probe.keys.clear();
        for (const SortSpec& s : node_.sort) probe.keys.push_back(scanner.cols[s.expr->index].get(i));
        if (heap_.size() == k && !less(probe, heap_.front())) continue;
        filter.materialize(scanner.cols, i, probe.row);
        if (heap_.size() == k) {
          std::pop_heap(heap_.begin(), heap_.end(), cmp());
          heap_.back() = std::move(probe);
        } else {
          heap_.push_back(std::move(probe));
        }
        std::push_heap(heap_.begin(), heap_.end(), cmp());
        probe = Entry();
      }
    }
    std::sort_heap(heap_.begin(), heap_.end(), cmp());
    scan.actual_rows = passed;
    scan.executed = true;
    pos_ = 0;
  }

  bool next(Row* row) override {
    if (pos_ >= heap_.size()) return false;
    *row = std::move(heap_[pos_++].row);
    return true;
  }

 private:
  struct Entry {
    std::vector<Value> keys;
    Row row;
  };

  bool less(const Entry& a, const Entry& b) const {
    for (size_t s = 0; s < node_.sort.size(); s++) {
      int c = compare(a.keys[s], b.keys[s]);
      if (c != 0) return node_.sort[s].desc ? c > 0 : c < 0;
    }
    return false;
  }

  struct Cmp {
    const VecTopNOp* op;
    bool operator()(const Entry& a, const Entry& b) const { return op->less(a, b); }
  };
  Cmp cmp() const { return Cmp{this}; }

  // Quick test on the first sort key only: can row i possibly beat the
  // current k-th best? (A tie on the first key needs the full comparison.)
  bool beatsWorst(const std::vector<ColumnVector>& cols, uint32_t i) const {
    const SortSpec& s = node_.sort[0];
    const ColumnVector& c = cols[s.expr->index];
    const Value& worst = heap_.front().keys[0];
    int cmp;
    if (c.nulls[i]) {
      cmp = worst.isNull() ? 0 : -1;
    } else if (worst.isNull()) {
      cmp = 1;
    } else if (c.type == Type::Float) {
      double a = c.floats[i], b = worst.asNumber();
      cmp = a < b ? -1 : a > b ? 1 : 0;
    } else if (c.type == Type::Int) {
      if (worst.type() == Type::Int) {
        cmp = c.ints[i] < worst.asInt() ? -1 : c.ints[i] > worst.asInt() ? 1 : 0;
      } else {
        double a = static_cast<double>(c.ints[i]), b = worst.asNumber();
        cmp = a < b ? -1 : a > b ? 1 : 0;
      }
    } else if (c.type == Type::Text) {
      int r = c.textAt(i).compare(worst.asText());
      cmp = r < 0 ? -1 : r > 0 ? 1 : 0;
    } else {
      return true;
    }
    if (cmp == 0) return true;  // decide with the full key
    return s.desc ? cmp > 0 : cmp < 0;
  }

  ExecContext& ctx_;
  std::vector<Entry> heap_;  // max-heap by sort order: front() is the current k-th best
  size_t pos_ = 0;
};

bool isPlainColumn(const ExprPtr& e) { return e && e->kind == ExprKind::Column; }

bool vectorizable(const PNode& p) {
  if (p.kids.size() != 1) return false;
  const PNode& scan = *p.kids[0];
  if (scan.kind != PKind::TableScan || scan.point || scan.index || scan.reverse) return false;
  if (p.kind == PKind::HashAggregate) {
    if (p.exprs.size() > 1) return false;
    if (p.exprs.size() == 1) {
      const ExprPtr& k = p.exprs[0];
      bool bucket = k->kind == ExprKind::Call && k->name == "bucket" && isPlainColumn(k->args[0]) &&
                    k->args[0]->type == Type::Int && k->args[1]->kind == ExprKind::Literal && !k->args[1]->now &&
                    k->args[1]->value.type() == Type::Int;
      if (!bucket) {
        if (!isPlainColumn(k)) return false;
        if (k->type != Type::Int && k->type != Type::Text) return false;
      }
    }
    for (const AggSpec& a : p.aggs) {
      bool pct = a.fn == "median" || a.fn == "p90" || a.fn == "p95" || a.fn == "p99";
      if (a.fn != "count" && a.fn != "sum" && a.fn != "avg" && a.fn != "min" && a.fn != "max" &&
          a.fn != "count_distinct" && !pct) {
        return false;
      }
      if (!a.arg) {
        if (a.fn != "count") return false;
        continue;
      }
      if (!isPlainColumn(a.arg)) return false;
      Type t = a.arg->type;
      if ((a.fn == "sum" || a.fn == "avg" || pct) && t != Type::Int && t != Type::Float) return false;
      if ((a.fn == "min" || a.fn == "max") && t != Type::Int && t != Type::Float && t != Type::Text) return false;
      if (a.fn == "count_distinct" && t != Type::Int && t != Type::Text) return false;
    }
    return true;
  }
  if (p.kind == PKind::TopN) {
    if (p.limit < 0) return false;
    for (const SortSpec& s : p.sort) {
      if (!isPlainColumn(s.expr)) return false;
      Type t = s.expr->type;
      if (t != Type::Int && t != Type::Float && t != Type::Text) return false;
    }
    return !p.sort.empty();
  }
  return false;
}

}  // namespace

void markVectorized(PNode& p) {
  p.vectorized = vectorizable(p);
  if (p.vectorized) return;  // its scan child is consumed by the vectorized operator
  for (const PPtr& k : p.kids) markVectorized(*k);
}

std::unique_ptr<Operator> buildVectorizedOperator(PNode& p, ExecContext& ctx) {
  if (p.kind == PKind::HashAggregate) return std::make_unique<VecAggregateOp>(p, ctx);
  return std::make_unique<VecTopNOp>(p, ctx);
}

}  // namespace pkql
