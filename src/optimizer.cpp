// The query optimizer: rule-based rewrites on the logical plan, then
// cost-based physical planning (access paths, join order, join algorithm,
// interesting orders), in the tradition of System R (Selinger et al., 1979).

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

#include "pkql/plan.h"
#include "pkql/vector.h"

namespace pkql {

namespace {

// Cost units are "work to read and decode one row sequentially". The other
// constants are relative to that and were calibrated by measurement on a
// warm cache (see README): a sequential row costs about 140 ns, a random
// primary-key lookup about 1.4 us, so one lookup is worth about 10 rows.
constexpr double kSeqRow = 1.0;
constexpr double kLookup = 10.0;
constexpr double kIndexEntry = 0.7;
constexpr double kSkipRow = 0.5;  // passing over a row without decoding it (sorted index fetch)
constexpr double kBatchedLookup = 3.0;  // one key of a sorted, batched lookup (multiGetRaw)
constexpr double kKeySort = 1.2;        // sorting one row key for a sorted index fetch
constexpr double kPred = 0.05;
constexpr double kHashBuild = 1.5;
constexpr double kHashProbe = 1.0;
constexpr double kNestedPair = 0.05;
constexpr double kProject = 0.02;
constexpr double kOut = 0.02;
constexpr double kSortFactor = 0.15;
constexpr double kDefaultEq = 0.1;      // selectivity of `col == x` with no statistics
constexpr double kDefaultRange = 1.0 / 3;
constexpr int kMaxDpRelations = 10;

// ===========================================================================
// Rewrite phase
// ===========================================================================

LPtr copyNode(const LPtr& n) { return std::make_shared<LNode>(*n); }

LPtr makeFilter(const LPtr& child, const std::vector<ExprPtr>& conjs) {
  if (conjs.empty()) return child;
  auto f = std::make_shared<LNode>();
  f->kind = LKind::Filter;
  f->schema = child->schema;
  f->kids = {child};
  f->pred = makeConjunction(conjs);
  return f;
}

bool isLiteralTrue(const Expr& e) { return e.kind == ExprKind::Literal && isTrue(e.value); }

// Places each conjunct as close to the data as possible.
LPtr pushFilter(const LPtr& child, std::vector<ExprPtr> conjs) {
  conjs.erase(std::remove_if(conjs.begin(), conjs.end(), [](const ExprPtr& c) { return isLiteralTrue(*c); }),
              conjs.end());
  if (conjs.empty()) return child;

  switch (child->kind) {
    case LKind::Filter: {
      splitConjuncts(child->pred, conjs);
      return pushFilter(child->kids[0], conjs);
    }
    case LKind::Project: {
      // Rewrite the predicate in terms of the projection's input.
      std::vector<ExprPtr> below;
      for (const ExprPtr& c : conjs) below.push_back(foldConstants(substituteColumns(c, child->exprs)));
      LPtr p = copyNode(child);
      p->kids[0] = pushFilter(child->kids[0], below);
      return p;
    }
    case LKind::Sort:
    case LKind::Distinct: {
      LPtr p = copyNode(child);
      p->kids[0] = pushFilter(child->kids[0], conjs);
      return p;
    }
    case LKind::Join: {
      int w = static_cast<int>(child->kids[0]->schema.size());
      int total = static_cast<int>(child->schema.size());
      std::vector<ExprPtr> left, right, mixed, stay;
      for (const ExprPtr& c : conjs) {
        if (usesOnlyRange(*c, 0, w)) {
          left.push_back(c);
        } else if (!child->left_join && usesOnlyRange(*c, w, total)) {
          right.push_back(shiftColumns(c, -w));
        } else if (!child->left_join) {
          mixed.push_back(c);
        } else {
          stay.push_back(c);  // above a left join, conditions on the right side must stay above it
        }
      }
      LPtr j = copyNode(child);
      j->kids[0] = pushFilter(child->kids[0], left);
      j->kids[1] = pushFilter(child->kids[1], right);
      if (!mixed.empty()) {
        std::vector<ExprPtr> all;
        splitConjuncts(child->pred, all);
        all.insert(all.end(), mixed.begin(), mixed.end());
        j->pred = makeConjunction(all);
      }
      return makeFilter(j, stay);
    }
    case LKind::Aggregate: {
      // A filter on grouping keys can run before grouping (HAVING -> WHERE).
      int nkeys = static_cast<int>(child->exprs.size());
      std::vector<ExprPtr> below, stay;
      for (const ExprPtr& c : conjs) {
        if (nkeys > 0 && usesOnlyRange(*c, 0, nkeys)) {
          below.push_back(foldConstants(substituteColumns(c, child->exprs)));
        } else {
          stay.push_back(c);
        }
      }
      LPtr a = copyNode(child);
      a->kids[0] = pushFilter(child->kids[0], below);
      return makeFilter(a, stay);
    }
    default:
      return makeFilter(child, conjs);
  }
}

LPtr rewrite(const LPtr& n) {
  LPtr out = copyNode(n);
  for (LPtr& k : out->kids) k = rewrite(k);

  if (out->kind == LKind::Filter) {
    std::vector<ExprPtr> conjs;
    splitConjuncts(out->pred, conjs);
    return pushFilter(out->kids[0], conjs);
  }
  if (out->kind == LKind::Join) {
    // Conditions in `on` that mention only one side are filters on that side.
    int w = static_cast<int>(out->kids[0]->schema.size());
    int total = static_cast<int>(out->schema.size());
    std::vector<ExprPtr> conjs, left, right, keep;
    splitConjuncts(out->pred, conjs);
    for (const ExprPtr& c : conjs) {
      std::vector<int> cols;
      collectColumns(*c, cols);
      if (!out->left_join && !cols.empty() && usesOnlyRange(*c, 0, w)) {
        left.push_back(c);
      } else if (!cols.empty() && usesOnlyRange(*c, w, total)) {
        right.push_back(shiftColumns(c, -w));  // also valid for left joins
      } else {
        keep.push_back(c);
      }
    }
    out->kids[0] = pushFilter(out->kids[0], left);
    out->kids[1] = pushFilter(out->kids[1], right);
    out->pred = keep.empty() ? Expr::literal(Value::boolean(true)) : makeConjunction(keep);
  }
  return out;
}

// ===========================================================================
// Selectivity estimation
// ===========================================================================

double clamp01(double x) { return std::max(0.0, std::min(1.0, x)); }

// Fraction of rows with value < v according to an equi-depth histogram.
double fractionBelow(const std::vector<Value>& bounds, const Value& v) {
  if (bounds.size() < 2) return kDefaultRange;
  if (compare(v, bounds.front()) <= 0) return 0;
  if (compare(v, bounds.back()) > 0) return 1;
  size_t buckets = bounds.size() - 1;
  for (size_t i = 0; i < buckets; i++) {
    if (compare(v, bounds[i + 1]) <= 0) {
      double within = 0.5;
      if (v.isNumber() && bounds[i].isNumber() && bounds[i + 1].isNumber()) {
        double lo = bounds[i].asNumber(), hi = bounds[i + 1].asNumber();
        within = hi > lo ? (v.asNumber() - lo) / (hi - lo) : 0.5;
      }
      return (static_cast<double>(i) + clamp01(within)) / static_cast<double>(buckets);
    }
  }
  return 1;
}

std::string successorText(const std::string& p) {
  std::string s = p;
  while (!s.empty()) {
    unsigned char c = static_cast<unsigned char>(s.back());
    if (c != 0xFF) {
      s.back() = static_cast<char>(c + 1);
      return s;
    }
    s.pop_back();
  }
  return s;
}

struct Bound {  // column op literal, normalized so the column is on the left
  int col = -1;
  Op op = Op::Eq;
  Value value;
  int param = -1;  // plan-cache parameter slot of the literal
};

Op flip(Op op) {
  switch (op) {
    case Op::Lt: return Op::Gt;
    case Op::Le: return Op::Ge;
    case Op::Gt: return Op::Lt;
    case Op::Ge: return Op::Le;
    default: return op;
  }
}

bool isComparison(Op op) {
  return op == Op::Eq || op == Op::Ne || op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge;
}

bool asBound(const Expr& e, Bound* b) {
  if (e.kind != ExprKind::Binary || !isComparison(e.op)) return false;
  const Expr& l = *e.args[0];
  const Expr& r = *e.args[1];
  if (l.kind == ExprKind::Column && r.kind == ExprKind::Literal) {
    *b = {l.index, e.op, r.value, r.param};
    return true;
  }
  if (r.kind == ExprKind::Column && l.kind == ExprKind::Literal) {
    *b = {r.index, flip(e.op), l.value, l.param};
    return true;
  }
  return false;
}

double eqSelectivity(const ColEstimate& c, double rows, const Value& v) {
  if (v.isNull()) return c.base && rows > 0 ? clamp01(static_cast<double>(c.base->nulls) / rows) : 0.05;
  if (c.unique) return rows > 0 ? 1.0 / rows : 1.0;
  if (c.ndv > 0) return 1.0 / c.ndv;
  return kDefaultEq;
}

double selectivity(const Expr& e, const std::vector<ColEstimate>& cols, double rows) {
  switch (e.kind) {
    case ExprKind::Literal: return isTrue(e.value) ? 1.0 : 0.0;
    case ExprKind::Column: return 0.5;  // a bool column
    case ExprKind::Unary:
      return e.op == Op::Not ? 1.0 - selectivity(*e.args[0], cols, rows) : 0.25;
    case ExprKind::Binary: {
      if (e.op == Op::And) return selectivity(*e.args[0], cols, rows) * selectivity(*e.args[1], cols, rows);
      if (e.op == Op::Or) {
        double a = selectivity(*e.args[0], cols, rows), b = selectivity(*e.args[1], cols, rows);
        return clamp01(a + b - a * b);
      }
      Bound b;
      if (asBound(e, &b)) {
        const ColEstimate& c = cols[b.col];
        switch (b.op) {
          case Op::Eq: return eqSelectivity(c, rows, b.value);
          case Op::Ne: return 1.0 - eqSelectivity(c, rows, b.value);
          default: {
            if (!c.base || c.base->bounds.size() < 2 || b.value.isNull()) return kDefaultRange;
            double below = fractionBelow(c.base->bounds, b.value);
            double s = (b.op == Op::Lt || b.op == Op::Le) ? below : 1.0 - below;
            return std::max(0.001, clamp01(s));
          }
        }
      }
      if (isComparison(e.op)) return e.op == Op::Eq ? kDefaultEq : kDefaultRange;
      return 0.25;
    }
    case ExprKind::InList: {
      if (e.args[0]->kind == ExprKind::Column) {
        const ColEstimate& c = cols[e.args[0]->index];
        double s = 0;
        for (size_t i = 1; i < e.args.size(); i++) {
          s += e.args[i]->kind == ExprKind::Literal ? eqSelectivity(c, rows, e.args[i]->value) : kDefaultEq;
        }
        return clamp01(s);
      }
      return clamp01(kDefaultEq * static_cast<double>(e.args.size() - 1));
    }
    case ExprKind::Call: {
      if (e.name == "starts_with" && e.args[0]->kind == ExprKind::Column && e.args[1]->kind == ExprKind::Literal &&
          e.args[1]->value.type() == Type::Text) {
        const ColEstimate& c = cols[e.args[0]->index];
        if (c.base && c.base->bounds.size() >= 2) {
          const std::string& p = e.args[1]->value.asText();
          double lo = fractionBelow(c.base->bounds, Value::text(p));
          double hi = fractionBelow(c.base->bounds, Value::text(successorText(p)));
          return std::max(0.001, clamp01(hi - lo));
        }
        return 0.05;
      }
      return 0.25;
    }
  }
  return 0.25;
}

double conjunctSelectivity(const std::vector<ExprPtr>& conjs, const std::vector<ColEstimate>& cols, double rows) {
  // Lower and upper bounds on the same column describe one range, so they
  // are estimated together (the histogram slice between them) rather than
  // multiplied as if independent: `id >= 50 and id < 55` is not
  // P(id >= 50) * P(id < 55).
  std::vector<Bound> bounds(conjs.size());
  std::vector<int> partner(conjs.size(), -1);  // index of the other bound of the same range
  std::map<int, std::pair<int, int>> ranges;    // column -> (lower conjunct, upper conjunct)
  for (size_t i = 0; i < conjs.size(); i++) {
    Bound& b = bounds[i];
    if (!asBound(*conjs[i], &b) || b.value.isNull() || !cols[b.col].base || cols[b.col].base->bounds.size() < 2) continue;
    auto it = ranges.emplace(b.col, std::make_pair(-1, -1)).first;
    if ((b.op == Op::Gt || b.op == Op::Ge) && it->second.first < 0) it->second.first = static_cast<int>(i);
    if ((b.op == Op::Lt || b.op == Op::Le) && it->second.second < 0) it->second.second = static_cast<int>(i);
  }
  for (const auto& r : ranges) {
    if (r.second.first >= 0 && r.second.second >= 0) {
      partner[r.second.first] = r.second.second;
      partner[r.second.second] = r.second.first;
    }
  }
  double s = 1;
  for (size_t i = 0; i < conjs.size(); i++) {
    if (partner[i] < 0) {
      s *= selectivity(*conjs[i], cols, rows);
    } else if (bounds[i].op == Op::Gt || bounds[i].op == Op::Ge) {  // count each pair once, at its lower bound
      const std::vector<Value>& h = cols[bounds[i].col].base->bounds;
      double slice = fractionBelow(h, bounds[partner[i]].value) - fractionBelow(h, bounds[i].value);
      s *= std::max(0.001, clamp01(slice));
    }
  }
  return s;
}

void capNdv(std::vector<ColEstimate>& cols, double rows) {
  for (ColEstimate& c : cols) {
    if (c.ndv > rows) c.ndv = std::max(1.0, rows);
  }
}

// ===========================================================================
// Physical planning
// ===========================================================================

bool startsWith(const std::vector<int>& order, const std::vector<int>& wanted) {
  if (wanted.empty() || order.size() < wanted.size()) return false;
  return std::equal(wanted.begin(), wanted.end(), order.begin());
}

double sortCost(double rows) { return kSortFactor * rows * std::log2(rows + 2); }

PPtr makeP(PKind kind) {
  auto p = std::make_shared<PNode>();
  p->kind = kind;
  return p;
}

// A base-table leaf: a scan with (after rewriting) at most one filter on top.
struct LeafInfo {
  const TableSchema* table = nullptr;
  std::vector<ExprPtr> conjs;
  Schema schema;
};

bool asBaseLeaf(const LPtr& n, LeafInfo* out) {
  if (n->kind == LKind::Scan) {
    *out = {n->table, {}, n->schema};
    return true;
  }
  if (n->kind == LKind::Filter && n->kids[0]->kind == LKind::Scan) {
    *out = {n->kids[0]->table, {}, n->schema};
    splitConjuncts(n->pred, out->conjs);
    return true;
  }
  return false;
}

std::vector<ColEstimate> tableEstimates(const TableSchema& t) {
  std::vector<ColEstimate> cols(t.cols.size());
  for (size_t i = 0; i < t.cols.size(); i++) {
    if (t.stats.analyzed && i < t.stats.cols.size()) {
      cols[i].ndv = static_cast<double>(t.stats.cols[i].ndv);
      cols[i].base = &t.stats.cols[i];
    }
    cols[i].unique = t.pk.size() == 1 && t.pk[0] == static_cast<int>(i);
    if (cols[i].unique) cols[i].ndv = static_cast<double>(std::max<uint64_t>(t.stats.rows, 1));
  }
  return cols;
}

double tableRows(const TableSchema& t) { return std::max(1.0, static_cast<double>(t.stats.rows)); }

// A literal usable as a key bound for a column of type `t` (encodings differ
// by type, so the literal must have the column's type exactly).
bool keyLiteral(Type col, Value* v) {
  if (v->isNull()) return true;
  if (col == Type::Float && v->type() == Type::Int) *v = v->castTo(Type::Float);
  return v->type() == col;
}

struct KeyMatch {
  std::vector<ExprPtr> eq;
  ExprPtr lo, hi;
  bool lo_incl = true, hi_incl = true;
  std::vector<bool> used;  // per conjunct
  int matched_cols = 0;    // leading key columns fixed by equality
  bool any = false;        // at least one conjunct narrows the range
};

// Matches conjuncts against the leading columns of a key: equalities on a
// prefix, then optionally a range (or a starts_with prefix) on the next column.
KeyMatch matchKey(const std::vector<int>& key_cols, const TableSchema& t, const std::vector<ExprPtr>& conjs) {
  KeyMatch m;
  m.used.assign(conjs.size(), false);
  for (int kc : key_cols) {
    Type type = t.cols[kc].type;
    bool found_eq = false;
    for (size_t i = 0; i < conjs.size() && !found_eq; i++) {
      Bound b;
      if (m.used[i] || !asBound(*conjs[i], &b) || b.col != kc || b.op != Op::Eq) continue;
      if (!keyLiteral(type, &b.value)) continue;
      m.eq.push_back(Expr::literal(b.value));
      m.eq.back()->param = b.param;
      m.used[i] = true;
      found_eq = true;
    }
    if (found_eq) {
      m.matched_cols++;
      m.any = true;
      continue;
    }
    // No equality on this column: look for a range, then stop.
    for (size_t i = 0; i < conjs.size(); i++) {
      if (m.used[i]) continue;
      const Expr& c = *conjs[i];
      Bound b;
      if (asBound(c, &b) && b.col == kc && b.op != Op::Eq && b.op != Op::Ne && !b.value.isNull() &&
          keyLiteral(type, &b.value)) {
        if ((b.op == Op::Gt || b.op == Op::Ge) && !m.lo) {
          m.lo = Expr::literal(b.value);
          m.lo_incl = b.op == Op::Ge;
          m.used[i] = true;
        } else if ((b.op == Op::Lt || b.op == Op::Le) && !m.hi) {
          m.hi = Expr::literal(b.value);
          m.hi_incl = b.op == Op::Le;
          m.used[i] = true;
        }
      } else if (c.kind == ExprKind::Call && c.name == "starts_with" && type == Type::Text && !m.lo && !m.hi &&
                 c.args[0]->kind == ExprKind::Column && c.args[0]->index == kc &&
                 c.args[1]->kind == ExprKind::Literal && c.args[1]->value.type() == Type::Text) {
        // starts_with(col, "ab") is the range ["ab", "ac").
        const std::string& p = c.args[1]->value.asText();
        m.lo = Expr::literal(Value::text(p));
        m.lo_incl = true;
        std::string succ = successorText(p);
        if (!succ.empty()) {
          m.hi = Expr::literal(Value::text(succ));
          m.hi_incl = false;
        }
        m.used[i] = true;
      }
    }
    if (m.lo || m.hi) m.any = true;
    break;
  }
  return m;
}

class Planner {
 public:
  explicit Planner(const PlannerOptions& o) : opts_(o) {}

  PPtr plan(const LPtr& n, const std::vector<int>* wanted = nullptr);

 private:
  PPtr planAccess(const LeafInfo& leaf, const std::vector<int>* wanted);
  PPtr planJoinRegion(const LPtr& n);
  PPtr planLeftJoin(const LPtr& n);
  PPtr planSort(const LPtr& sort, const LPtr& limit);

  struct Rel {
    PPtr plan;
    std::vector<int> map;  // region column -> position in plan output (-1 = absent)
    uint32_t mask = 0;
    const LeafInfo* leaf = nullptr;  // set when this is a single base-table leaf
  };
  struct Cond {
    ExprPtr expr;  // bound to region columns
    uint32_t mask = 0;
  };
  Rel joinPair(const Rel& a, const Rel& b, const std::vector<const Cond*>& conds, bool left_join,
               int region_width);

  PlannerOptions opts_;
};

PPtr Planner::planAccess(const LeafInfo& leaf, const std::vector<int>* wanted) {
  const TableSchema& t = *leaf.table;
  double n = tableRows(t);
  std::vector<ColEstimate> cols = tableEstimates(t);
  double out_rows = std::max(1.0, n * conjunctSelectivity(leaf.conjs, cols, n));

  struct Candidate {
    const IndexSchema* index;
    std::vector<int> key_cols;
  };
  std::vector<Candidate> cands = {{nullptr, t.pk}};
  if (opts_.use_indexes) {
    for (const IndexSchema& ix : t.indexes) cands.push_back({&ix, ix.cols});
  }

  PPtr best, best_ordered;
  for (const Candidate& cand : cands) {
    KeyMatch m = matchKey(cand.key_cols, t, leaf.conjs);
    std::vector<int> order(cand.key_cols.begin() + m.matched_cols, cand.key_cols.end());
    if (cand.index) order.insert(order.end(), t.pk.begin(), t.pk.end());
    bool gives_order = wanted && opts_.use_interesting_orders && startsWith(order, *wanted);
    // A secondary index that narrows nothing is only worth it for its order.
    if (cand.index && !m.any && !gives_order) continue;

    std::vector<ExprPtr> matched, residual;
    for (size_t i = 0; i < leaf.conjs.size(); i++) (m.used[i] ? matched : residual).push_back(leaf.conjs[i]);
    double scanned = std::max(1.0, n * conjunctSelectivity(matched, cols, n));

    auto p = makeP(cand.index ? PKind::IndexScan : PKind::TableScan);
    p->table = &t;
    p->index = cand.index;
    p->eq = m.eq;
    p->lo = m.lo;
    p->hi = m.hi;
    p->lo_incl = m.lo_incl;
    p->hi_incl = m.hi_incl;
    p->point = !cand.index && m.matched_cols == static_cast<int>(t.pk.size()) && !m.lo && !m.hi;
    p->pred = makeConjunction(residual);
    p->schema = leaf.schema;
    p->rows = p->point ? std::min(out_rows, 1.0) : out_rows;
    if (cand.index) {
      // Fetch matches one lookup each, or (sorted index fetch) sort their
      // row keys and then look them up in batches, or read the table in one
      // pass that skips the other rows: whichever is cheapest. Equality on
      // every index column yields the row keys already sorted.
      bool presorted = m.matched_cols == static_cast<int>(cand.key_cols.size()) && !m.lo && !m.hi;
      double key_sort = presorted ? 0.0 : scanned * kKeySort;
      double batched = scanned * (kBatchedLookup + kSeqRow) + key_sort;
      double skipping = n * kSkipRow + scanned * kSeqRow + key_sort;
      double fetch = std::min({scanned * kLookup, batched, skipping});
      p->cost = kLookup + scanned * kIndexEntry + fetch;
    } else {
      p->cost = p->point ? kLookup : kLookup + scanned * kSeqRow;
    }
    p->cost += (p->point ? 1.0 : scanned) * kPred * static_cast<double>(residual.size());
    p->order = order;
    p->cols = cols;
    capNdv(p->cols, p->rows);

    if (!best || p->cost < best->cost) best = p;
    if (gives_order && (!best_ordered || p->cost < best_ordered->cost)) best_ordered = p;
  }
  return best_ordered ? best_ordered : best;
}

Planner::Rel Planner::joinPair(const Rel& a, const Rel& b, const std::vector<const Cond*>& conds, bool left_join,
                               int region_width) {
  int wa = static_cast<int>(a.plan->schema.size());
  std::vector<int> layout(region_width, -1);
  for (int g = 0; g < region_width; g++) {
    if (a.map[g] >= 0) layout[g] = a.map[g];
    else if (b.map[g] >= 0) layout[g] = b.map[g] + wa;
  }

  Schema schema = a.plan->schema;
  schema.insert(schema.end(), b.plan->schema.begin(), b.plan->schema.end());
  std::vector<ColEstimate> cols = a.plan->cols;
  cols.insert(cols.end(), b.plan->cols.begin(), b.plan->cols.end());

  double sel = 1;
  std::vector<ExprPtr> left_keys, right_keys, residual;
  for (const Cond* c : conds) {
    ExprPtr e = remapColumns(c->expr, layout);
    int total = static_cast<int>(schema.size());
    bool equi = false;
    if (e->kind == ExprKind::Binary && e->op == Op::Eq) {
      const ExprPtr& l = e->args[0];
      const ExprPtr& r = e->args[1];
      ExprPtr lk, rk;
      if (usesOnlyRange(*l, 0, wa) && usesOnlyRange(*r, wa, total) && !isConstant(*l) && !isConstant(*r)) {
        lk = l;
        rk = r;
      } else if (usesOnlyRange(*r, 0, wa) && usesOnlyRange(*l, wa, total) && !isConstant(*l) && !isConstant(*r)) {
        lk = r;
        rk = l;
      }
      if (lk) {
        equi = true;
        left_keys.push_back(lk);
        right_keys.push_back(shiftColumns(rk, -wa));
        // Selinger: |A join B| = |A| |B| / max(ndv(a), ndv(b)), where an
        // unknown ndv defaults to that side's row count (a key-like column).
        double na = a.plan->rows, nb = b.plan->rows;
        if (lk->kind == ExprKind::Column && cols[lk->index].ndv > 0) na = cols[lk->index].ndv;
        if (rk->kind == ExprKind::Column && cols[rk->index].ndv > 0) nb = cols[rk->index].ndv;
        sel *= 1.0 / std::max(1.0, std::max(na, nb));
      }
    }
    if (!equi) {
      residual.push_back(e);
      sel *= selectivity(*e, cols, a.plan->rows * b.plan->rows);
    }
  }

  double joined = std::max(1.0, a.plan->rows * b.plan->rows * sel);
  double out_rows = left_join ? std::max(joined, a.plan->rows) : joined;
  double pred_cost = kPred * static_cast<double>(residual.size());

  Rel best;
  best.mask = a.mask | b.mask;
  best.map.assign(region_width, -1);
  for (int g = 0; g < region_width; g++) best.map[g] = layout[g];

  auto consider = [&](const PPtr& p) {
    p->schema = schema;
    p->rows = out_rows;
    p->left_join = left_join;
    p->cols = cols;
    capNdv(p->cols, out_rows);
    if (!best.plan || p->cost < best.plan->cost) best.plan = p;
  };

  // 1. Hash join: build a hash table on B, probe it with A.
  if (!left_keys.empty()) {
    auto p = makeP(PKind::HashJoin);
    p->kids = {a.plan, b.plan};
    p->left_keys = left_keys;
    p->right_keys = right_keys;
    p->pred = makeConjunction(residual);
    p->startup = b.plan->cost + b.plan->rows * kHashBuild;
    p->cost = a.plan->cost + p->startup + a.plan->rows * kHashProbe + joined * (kOut + pred_cost);
    p->order = a.plan->order;  // probe order is preserved
    consider(p);
  }

  // 2. Index nested-loop join: B is a base table; look it up by key for
  //    each row of A instead of reading it in full.
  if (b.leaf && opts_.use_indexes && !left_keys.empty()) {
    const TableSchema& t = *b.leaf->table;
    std::vector<std::pair<const IndexSchema*, std::vector<int>>> keys = {{nullptr, t.pk}};
    for (const IndexSchema& ix : t.indexes) keys.push_back({&ix, ix.cols});
    for (const auto& key : keys) {
      std::vector<ExprPtr> eq;
      std::vector<bool> used(right_keys.size(), false);
      for (int kc : key.second) {
        bool found = false;
        for (size_t i = 0; i < right_keys.size() && !found; i++) {
          if (!used[i] && right_keys[i]->kind == ExprKind::Column && right_keys[i]->index == kc &&
              left_keys[i]->type == t.cols[kc].type) {
            eq.push_back(left_keys[i]);
            used[i] = true;
            found = true;
          }
        }
        if (!found) break;
      }
      if (eq.empty()) continue;

      std::vector<ExprPtr> res = residual;
      for (size_t i = 0; i < right_keys.size(); i++) {
        if (!used[i]) {
          auto e = Expr::binary(Op::Eq, left_keys[i], shiftColumns(right_keys[i], wa));
          e->type = Type::Bool;
          e->nullable = false;
          res.push_back(e);
        }
      }
      for (const ExprPtr& c : b.leaf->conjs) res.push_back(shiftColumns(c, wa));

      std::vector<ColEstimate> tcols = tableEstimates(t);
      double n = tableRows(t);
      bool unique = !key.first && eq.size() == t.pk.size();
      double matches = unique ? 1.0 : n;
      if (!unique) {
        for (size_t i = 0; i < eq.size(); i++) {
          const ColEstimate& c = tcols[key.second[i]];
          matches /= c.ndv > 0 ? c.ndv : 1.0 / kDefaultEq;
        }
        matches = std::max(matches, 1.0);
      }
      double per_match = key.first ? kIndexEntry + kLookup : kSeqRow;

      auto p = makeP(PKind::IndexJoin);
      p->kids = {a.plan};
      p->table = &t;
      p->index = key.first;
      p->eq = eq;
      p->point = unique;
      p->pred = makeConjunction(res);
      p->cost = a.plan->cost + a.plan->rows * (kLookup + matches * per_match) +
                joined * (kOut + kPred * static_cast<double>(res.size()));
      p->order = a.plan->order;
      consider(p);
    }
  }

  // 3. Nested loops: always possible (any condition), quadratic.
  {
    auto p = makeP(PKind::NestedLoopJoin);
    p->kids = {a.plan, b.plan};
    std::vector<ExprPtr> all = residual;
    for (size_t i = 0; i < left_keys.size(); i++) {
      auto e = Expr::binary(Op::Eq, left_keys[i], shiftColumns(right_keys[i], wa));
      e->type = Type::Bool;
      e->nullable = false;
      all.push_back(e);
    }
    p->pred = makeConjunction(all);
    p->startup = b.plan->cost;
    p->cost = a.plan->cost + b.plan->cost + a.plan->rows * b.plan->rows * kNestedPair + joined * kOut;
    p->order = a.plan->order;
    consider(p);
  }
  return best;
}

PPtr Planner::planJoinRegion(const LPtr& root) {
  // Flatten the tree of inner joins into leaves and conditions.
  struct LeafCtx {
    LPtr node;
    int offset;
    LeafInfo info;
    bool base;
  };
  std::vector<LeafCtx> leaves;
  std::vector<Cond> conds;
  std::vector<std::pair<ExprPtr, int>> raw_conds;  // (condition, offset of its join)

  std::function<void(const LPtr&, int)> flatten = [&](const LPtr& n, int offset) {
    if (n->kind == LKind::Join && !n->left_join) {
      flatten(n->kids[0], offset);
      flatten(n->kids[1], offset + static_cast<int>(n->kids[0]->schema.size()));
      std::vector<ExprPtr> parts;
      splitConjuncts(n->pred, parts);
      for (const ExprPtr& p : parts) {
        if (!isLiteralTrue(*p)) raw_conds.push_back({p, offset});
      }
      return;
    }
    LeafCtx l{n, offset, {}, false};
    l.base = asBaseLeaf(n, &l.info);
    leaves.push_back(l);
  };
  flatten(root, 0);

  int width = static_cast<int>(root->schema.size());
  std::vector<int> col_leaf(width, -1);
  for (size_t i = 0; i < leaves.size(); i++) {
    int w = static_cast<int>(leaves[i].node->schema.size());
    for (int c = 0; c < w; c++) col_leaf[leaves[i].offset + c] = static_cast<int>(i);
  }
  for (const auto& rc : raw_conds) {
    Cond c;
    c.expr = shiftColumns(rc.first, rc.second);
    std::vector<int> cols;
    collectColumns(*c.expr, cols);
    for (int col : cols) c.mask |= 1u << col_leaf[col];
    conds.push_back(c);
  }

  size_t n = leaves.size();
  std::vector<Rel> singles(n);
  for (size_t i = 0; i < n; i++) {
    Rel& r = singles[i];
    r.plan = leaves[i].base ? planAccess(leaves[i].info, nullptr) : plan(leaves[i].node);
    r.mask = 1u << i;
    r.map.assign(width, -1);
    int w = static_cast<int>(leaves[i].node->schema.size());
    for (int c = 0; c < w; c++) r.map[leaves[i].offset + c] = c;
    r.leaf = leaves[i].base ? &leaves[i].info : nullptr;
  }

  auto condsBetween = [&](uint32_t a, uint32_t b) {
    std::vector<const Cond*> out;
    for (const Cond& c : conds) {
      if ((c.mask & ~(a | b)) == 0 && (c.mask & a) && (c.mask & b)) out.push_back(&c);
    }
    return out;
  };

  Rel result;
  if (!opts_.reorder_joins || n > kMaxDpRelations) {
    // Left-deep, in the order the joins were written.
    result = singles[0];
    for (size_t i = 1; i < n; i++) result = joinPair(result, singles[i], condsBetween(result.mask, singles[i].mask), false, width);
  } else {
    // Dynamic programming over subsets (bushy plans allowed): the cheapest
    // plan for each set of relations is built from the cheapest plans of
    // its two halves.
    std::map<uint32_t, Rel> best;
    for (size_t i = 0; i < n; i++) best[1u << i] = singles[i];
    uint32_t full = (1u << n) - 1;
    for (size_t size = 2; size <= n; size++) {
      for (uint32_t mask = 1; mask <= full; mask++) {
        if (static_cast<size_t>(__builtin_popcount(mask)) != size) continue;
        for (int pass = 0; pass < 2 && !best.count(mask); pass++) {
          // Pass 0 joins only connected halves; pass 1 allows a cross product
          // when the relations are not connected by any condition.
          for (uint32_t a = (mask - 1) & mask; a > 0; a = (a - 1) & mask) {
            uint32_t b = mask ^ a;
            auto ia = best.find(a), ib = best.find(b);
            if (ia == best.end() || ib == best.end()) continue;
            std::vector<const Cond*> cs = condsBetween(a, b);
            if (cs.empty() && pass == 0) continue;
            Rel r = joinPair(ia->second, ib->second, cs, false, width);
            auto cur = best.find(mask);
            if (cur == best.end() || r.plan->cost < cur->second.plan->cost) best[mask] = r;
          }
        }
      }
    }
    result = best[full];
  }

  // Restore the column order the rest of the plan expects.
  bool identity = static_cast<int>(result.plan->schema.size()) == width;
  for (int g = 0; g < width && identity; g++) identity = result.map[g] == g;
  if (identity) return result.plan;
  auto p = makeP(PKind::Project);
  p->kids = {result.plan};
  for (int g = 0; g < width; g++) {
    auto c = Expr::column(root->schema[g].qualifier, root->schema[g].name);
    c->index = result.map[g];
    c->type = root->schema[g].type;
    c->nullable = root->schema[g].nullable;
    p->exprs.push_back(c);
    p->cols.push_back(result.plan->cols[result.map[g]]);
  }
  p->schema = root->schema;
  p->rows = result.plan->rows;
  p->cost = result.plan->cost;
  p->startup = result.plan->startup;
  return p;
}

PPtr Planner::planLeftJoin(const LPtr& n) {
  int wl = static_cast<int>(n->kids[0]->schema.size());
  int width = static_cast<int>(n->schema.size());
  LeafInfo right_leaf;
  bool right_base = asBaseLeaf(n->kids[1], &right_leaf);

  Rel a, b;
  a.plan = plan(n->kids[0]);
  a.mask = 1;
  a.map.assign(width, -1);
  for (int c = 0; c < wl; c++) a.map[c] = c;
  b.plan = right_base ? planAccess(right_leaf, nullptr) : plan(n->kids[1]);
  b.mask = 2;
  b.map.assign(width, -1);
  for (int c = wl; c < width; c++) b.map[c] = c - wl;
  b.leaf = right_base ? &right_leaf : nullptr;

  std::vector<Cond> conds;
  std::vector<ExprPtr> parts;
  splitConjuncts(n->pred, parts);
  for (const ExprPtr& p : parts) {
    if (!isLiteralTrue(*p)) conds.push_back({p, 3});
  }
  std::vector<const Cond*> cs;
  for (const Cond& c : conds) cs.push_back(&c);
  Rel r = joinPair(a, b, cs, true, width);
  for (int c = wl; c < width; c++) r.plan->schema[c].nullable = true;
  return r.plan;
}

// Records that the plan's output order is relied on, down to the scan that
// produces it, so the executor keeps that order (no sorted index fetch).
void markOrderNeeded(PNode& p) {
  p.order_needed = true;
  switch (p.kind) {
    case PKind::Filter:
    case PKind::Project:
    case PKind::Limit:
    case PKind::HashJoin:
    case PKind::IndexJoin:
    case PKind::NestedLoopJoin:
      markOrderNeeded(*p.kids[0]);  // these pass their (first) input's order through
      break;
    default:
      break;
  }
}

PPtr Planner::planSort(const LPtr& sort, const LPtr& limit) {
  const LPtr& in = sort->kids[0];
  std::vector<int> wanted;
  bool simple = opts_.use_interesting_orders;
  for (const SortSpec& s : sort->sort) {
    if (s.desc || s.expr->kind != ExprKind::Column) simple = false;
    else wanted.push_back(s.expr->index);
  }

  PPtr any = plan(in);
  int64_t k = limit ? (limit->limit < 0 ? -1 : limit->limit + limit->offset) : -1;

  // Option A: sort (or top-N) the cheapest input.
  PPtr sorted;
  if (simple && startsWith(any->order, wanted)) {
    sorted = any;  // already in order
    markOrderNeeded(*sorted);
  } else {
    sorted = makeP(k >= 0 ? PKind::TopN : PKind::Sort);
    sorted->kids = {any};
    sorted->sort = sort->sort;
    sorted->limit = k;
    sorted->schema = any->schema;
    sorted->rows = k >= 0 ? std::min(any->rows, static_cast<double>(k)) : any->rows;
    double work = k >= 0 ? kSortFactor * any->rows * std::log2(static_cast<double>(k) + 2) : sortCost(any->rows);
    sorted->cost = any->cost + work;
    sorted->startup = sorted->cost;
    sorted->cols = any->cols;
    sorted->order = simple ? wanted : std::vector<int>();
  }

  // Option B: an input that already arrives in the wanted order, e.g. an
  // index on the sort column. With a limit it can stop after k rows.
  if (simple) {
    PPtr ordered = plan(in, &wanted);
    if (startsWith(ordered->order, wanted)) {
      double cost_a = sorted->cost, cost_b = ordered->cost;
      if (k >= 0) {
        double frac = std::min(1.0, static_cast<double>(k) / std::max(1.0, ordered->rows));
        cost_b = ordered->startup + (ordered->cost - ordered->startup) * frac;
      }
      if (cost_b < cost_a) {
        sorted = ordered;
        markOrderNeeded(*sorted);
      }
    }
  }
  return sorted;
}

PPtr Planner::plan(const LPtr& n, const std::vector<int>* wanted) {
  LeafInfo leaf;
  if (asBaseLeaf(n, &leaf)) return planAccess(leaf, wanted);

  switch (n->kind) {
    case LKind::Scan: break;  // handled above

    case LKind::Filter: {
      PPtr child = plan(n->kids[0], wanted);
      auto p = makeP(PKind::Filter);
      p->kids = {child};
      p->pred = n->pred;
      p->schema = n->schema;
      std::vector<ExprPtr> conjs;
      splitConjuncts(n->pred, conjs);
      p->rows = std::max(1.0, child->rows * conjunctSelectivity(conjs, child->cols, child->rows));
      p->cost = child->cost + child->rows * kPred * static_cast<double>(conjs.size());
      p->startup = child->startup;
      p->order = child->order;
      p->cols = child->cols;
      capNdv(p->cols, p->rows);
      return p;
    }

    case LKind::Project: {
      // Map a wanted output order to the input's columns where the
      // projection passes columns through unchanged.
      std::vector<int> child_wanted;
      const std::vector<int>* cw = nullptr;
      if (wanted) {
        bool ok = true;
        for (int w : *wanted) {
          const ExprPtr& e = n->exprs[w];
          if (e->kind != ExprKind::Column) ok = false;
          else child_wanted.push_back(e->index);
        }
        if (ok) cw = &child_wanted;
      }
      PPtr child = plan(n->kids[0], cw);
      // Drop projections that pass every column through unchanged.
      bool identity = n->exprs.size() == child->schema.size();
      for (size_t i = 0; i < n->exprs.size() && identity; i++) {
        identity = n->exprs[i]->kind == ExprKind::Column && n->exprs[i]->index == static_cast<int>(i);
      }
      if (identity) {
        child->schema = n->schema;
        return child;
      }
      auto p = makeP(PKind::Project);
      p->kids = {child};
      p->exprs = n->exprs;
      p->schema = n->schema;
      p->rows = child->rows;
      p->cost = child->cost + child->rows * kProject * static_cast<double>(n->exprs.size());
      p->startup = child->startup;
      for (const ExprPtr& e : n->exprs) {
        p->cols.push_back(e->kind == ExprKind::Column ? child->cols[e->index] : ColEstimate());
      }
      for (int o : child->order) {  // order survives on passed-through columns
        int out = -1;
        for (size_t i = 0; i < n->exprs.size(); i++) {
          if (n->exprs[i]->kind == ExprKind::Column && n->exprs[i]->index == o) out = static_cast<int>(i);
        }
        if (out < 0) break;
        p->order.push_back(out);
      }
      return p;
    }

    case LKind::Join:
      return n->left_join ? planLeftJoin(n) : planJoinRegion(n);

    case LKind::Aggregate: {
      PPtr child = plan(n->kids[0]);
      auto p = makeP(PKind::HashAggregate);
      p->kids = {child};
      p->exprs = n->exprs;
      p->aggs = n->aggs;
      p->schema = n->schema;
      double groups = 1;
      if (!n->exprs.empty()) {
        bool known = true;
        for (const ExprPtr& e : n->exprs) {
          double ndv = e->kind == ExprKind::Column ? child->cols[e->index].ndv : -1;
          if (ndv > 0) groups *= ndv;
          else known = false;
        }
        if (!known) groups = std::max(groups, child->rows / 10);
        groups = std::max(1.0, std::min(groups, child->rows));
      }
      p->rows = groups;
      p->cost = child->cost + child->rows * kHashBuild;
      p->startup = p->cost;
      for (const ExprPtr& e : n->exprs) {
        ColEstimate c = e->kind == ExprKind::Column ? child->cols[e->index] : ColEstimate();
        c.unique = n->exprs.size() == 1;
        p->cols.push_back(c);
      }
      p->cols.resize(n->schema.size());
      return p;
    }

    case LKind::Sort:
      return planSort(n, nullptr);

    case LKind::Limit: {
      if (n->kids[0]->kind == LKind::Sort && n->limit >= 0) {
        PPtr sorted = planSort(n->kids[0], n);
        if (sorted->kind == PKind::TopN && n->offset == 0) return sorted;  // top-N already applies the limit
        auto p = makeP(PKind::Limit);
        p->kids = {sorted};
        p->limit = n->limit;
        p->offset = n->offset;
        p->schema = n->schema;
        p->rows = std::max(0.0, std::min(sorted->rows - static_cast<double>(n->offset), static_cast<double>(n->limit)));
        double frac = std::min(1.0, static_cast<double>(n->limit + n->offset) / std::max(1.0, sorted->rows));
        p->cost = sorted->startup + (sorted->cost - sorted->startup) * frac;
        p->startup = sorted->startup;
        p->order = sorted->order;
        p->cols = sorted->cols;
        return p;
      }
      PPtr child = plan(n->kids[0], wanted);
      auto p = makeP(PKind::Limit);
      p->kids = {child};
      p->limit = n->limit;
      p->offset = n->offset;
      p->schema = n->schema;
      double avail = std::max(0.0, child->rows - static_cast<double>(n->offset));
      p->rows = n->limit < 0 ? avail : std::min(avail, static_cast<double>(n->limit));
      double frac = n->limit < 0 ? 1.0
                                 : std::min(1.0, static_cast<double>(n->limit + n->offset) / std::max(1.0, child->rows));
      p->cost = child->startup + (child->cost - child->startup) * frac;
      p->startup = child->startup;
      p->order = child->order;
      p->cols = child->cols;
      return p;
    }

    case LKind::Distinct: {
      PPtr child = plan(n->kids[0]);
      auto p = makeP(PKind::Distinct);
      p->kids = {child};
      p->schema = n->schema;
      p->rows = child->schema.size() == 1 && child->cols[0].ndv > 0 ? child->cols[0].ndv : std::max(1.0, child->rows / 2);
      p->cost = child->cost + child->rows * kHashBuild;
      p->cols = child->cols;
      p->order = child->order;
      return p;
    }
  }
  throw Error("internal error: unplannable node");
}

// ===========================================================================
// Explain
// ===========================================================================

std::string keyColumnName(const PNode& p, size_t i) {
  const std::vector<int>& cols = p.index ? p.index->cols : p.table->pk;
  return i < cols.size() ? p.table->cols[cols[i]].name : "?";
}

std::string keyCondition(const PNode& p) {
  std::string s;
  for (size_t i = 0; i < p.eq.size(); i++) {
    s += (s.empty() ? "" : " and ") + keyColumnName(p, i) + " == " + toString(*p.eq[i]);
  }
  std::string next = keyColumnName(p, p.eq.size());
  if (p.lo) s += (s.empty() ? "" : " and ") + next + (p.lo_incl ? " >= " : " > ") + toString(*p.lo);
  if (p.hi) s += (s.empty() ? "" : " and ") + next + (p.hi_incl ? " <= " : " < ") + toString(*p.hi);
  return s;
}

std::string sortList(const std::vector<SortSpec>& sort) {
  std::string s;
  for (const SortSpec& k : sort) s += (s.empty() ? "" : ", ") + std::string(k.desc ? "-" : "") + toString(*k.expr);
  return s;
}

std::string describe(const PNode& p) {
  switch (p.kind) {
    case PKind::TableScan: {
      std::string s = p.point ? "PointLookup " : "TableScan ";
      s += p.table->name;
      std::string k = keyCondition(p);
      s += k.empty() ? " (full)" : " [" + k + "]";
      if (p.pred) s += " filter " + toString(*p.pred);
      return s;
    }
    case PKind::IndexScan: {
      std::string s = "IndexScan " + p.table->name + "." + p.index->name;
      std::string k = keyCondition(p);
      s += k.empty() ? " (full, for order)" : " [" + k + "]";
      if (p.pred) s += " filter " + toString(*p.pred);
      return s;
    }
    case PKind::Filter: return "Filter " + toString(*p.pred);
    case PKind::Project: {
      std::string s = "Project ";
      for (size_t i = 0; i < p.schema.size(); i++) s += (i ? ", " : "") + displayName(p.schema[i]);
      return s;
    }
    case PKind::HashJoin: {
      std::string s = std::string(p.left_join ? "HashLeftJoin" : "HashJoin") + " on ";
      for (size_t i = 0; i < p.left_keys.size(); i++) {
        s += (i ? " and " : "") + toString(*p.left_keys[i]) + " == " + toString(*p.right_keys[i]);
      }
      if (p.pred) s += " filter " + toString(*p.pred);
      return s;
    }
    case PKind::IndexJoin: {
      std::string s = std::string(p.left_join ? "IndexLeftJoin " : "IndexJoin ") + p.table->name + " via " +
                      (p.index ? p.index->name : std::string("primary key")) + " [";
      for (size_t i = 0; i < p.eq.size(); i++) s += (i ? " and " : "") + keyColumnName(p, i) + " == " + toString(*p.eq[i]);
      s += "]";
      if (p.pred) s += " filter " + toString(*p.pred);
      return s;
    }
    case PKind::NestedLoopJoin:
      return std::string(p.left_join ? "NestedLoopLeftJoin" : "NestedLoopJoin") +
             (p.pred ? " on " + toString(*p.pred) : std::string(" (cross product)"));
    case PKind::HashAggregate: {
      std::string s = "HashAggregate";
      if (!p.exprs.empty()) {
        s += " by ";
        for (size_t i = 0; i < p.exprs.size(); i++) s += (i ? ", " : "") + toString(*p.exprs[i]);
      }
      s += " {";
      for (size_t i = 0; i < p.aggs.size(); i++) {
        s += (i ? ", " : "") + p.aggs[i].fn + "(" + (p.aggs[i].arg ? toString(*p.aggs[i].arg) : "") + ")";
      }
      return s + "}";
    }
    case PKind::Sort: return "Sort by " + sortList(p.sort);
    case PKind::TopN: return "TopN " + std::to_string(p.limit) + " by " + sortList(p.sort);
    case PKind::Limit:
      return "Limit " + (p.limit < 0 ? std::string("all") : std::to_string(p.limit)) +
             (p.offset ? " skip " + std::to_string(p.offset) : "");
    case PKind::Distinct: return "Distinct";
  }
  return "?";
}

std::string fmt(double x) {
  char buf[32];
  if (x >= 100 || x == std::floor(x)) std::snprintf(buf, sizeof buf, "%.0f", x);
  else std::snprintf(buf, sizeof buf, "%.1f", x);
  return buf;
}

void explainRec(const PNode& p, const std::string& prefix, bool last, bool root, bool actuals, std::string& out) {
  out += prefix;
  if (!root) out += last ? "└─ " : "├─ ";
  out += describe(p);
  if (p.vectorized) out += " [vectorized]";
  out += "  (est " + fmt(p.rows) + " rows, cost " + fmt(p.cost) + ")";
  if (actuals && p.executed) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", p.actual_ms);
    out += "  [actual " + std::to_string(p.actual_rows) + " rows, " + buf + " ms]";
  }
  out += "\n";
  std::string child_prefix = prefix + (root ? "" : (last ? "   " : "│  "));
  for (size_t i = 0; i < p.kids.size(); i++) {
    explainRec(*p.kids[i], child_prefix, i + 1 == p.kids.size(), false, actuals, out);
  }
}

// ===========================================================================
// Column pruning: walk the finished plan top-down and record which columns
// each scan must actually decode.
// ===========================================================================

void markColumns(const ExprPtr& e, std::vector<bool>& req) {
  if (!e) return;
  std::vector<int> cols;
  collectColumns(*e, cols);
  for (int c : cols) req[c] = true;
}

void prune(PNode& p, std::vector<bool> req) {
  auto splitAt = [](const std::vector<bool>& all, size_t w, std::vector<bool>* l, std::vector<bool>* r) {
    l->assign(all.begin(), all.begin() + static_cast<long>(w));
    r->assign(all.begin() + static_cast<long>(w), all.end());
  };
  switch (p.kind) {
    case PKind::TableScan:
    case PKind::IndexScan:
      markColumns(p.pred, req);
      p.needed = req;
      return;
    case PKind::Filter:
      markColumns(p.pred, req);
      prune(*p.kids[0], req);
      return;
    case PKind::Project: {
      std::vector<bool> in(p.kids[0]->schema.size(), false);
      for (size_t i = 0; i < p.exprs.size(); i++) {
        if (req[i]) markColumns(p.exprs[i], in);
      }
      prune(*p.kids[0], in);
      return;
    }
    case PKind::HashJoin:
    case PKind::NestedLoopJoin: {
      markColumns(p.pred, req);
      std::vector<bool> l, r;
      splitAt(req, p.kids[0]->schema.size(), &l, &r);
      for (const ExprPtr& k : p.left_keys) markColumns(k, l);
      for (const ExprPtr& k : p.right_keys) markColumns(k, r);
      prune(*p.kids[0], l);
      prune(*p.kids[1], r);
      return;
    }
    case PKind::IndexJoin: {
      markColumns(p.pred, req);
      std::vector<bool> outer, inner;
      splitAt(req, p.kids[0]->schema.size(), &outer, &inner);
      for (const ExprPtr& e : p.eq) markColumns(e, outer);
      p.needed = inner;
      prune(*p.kids[0], outer);
      return;
    }
    case PKind::HashAggregate: {
      std::vector<bool> in(p.kids[0]->schema.size(), false);
      for (const ExprPtr& e : p.exprs) markColumns(e, in);
      for (const AggSpec& a : p.aggs) markColumns(a.arg, in);
      prune(*p.kids[0], in);
      return;
    }
    case PKind::Sort:
    case PKind::TopN:
      for (const SortSpec& s : p.sort) markColumns(s.expr, req);
      prune(*p.kids[0], req);
      return;
    case PKind::Limit:
      prune(*p.kids[0], req);
      return;
    case PKind::Distinct:
      prune(*p.kids[0], std::vector<bool>(p.kids[0]->schema.size(), true));
      return;
  }
}

}  // namespace

PPtr optimize(const LPtr& logical, const PlannerOptions& opts) {
  LPtr rewritten = rewrite(logical);
  PPtr plan = Planner(opts).plan(rewritten);
  prune(*plan, std::vector<bool>(plan->schema.size(), true));
  if (opts.vectorize) markVectorized(*plan);
  return plan;
}

std::string explainPlan(const PNode& root, bool with_actuals) {
  std::string out;
  explainRec(root, "", true, true, with_actuals, out);
  return out;
}

}  // namespace pkql
