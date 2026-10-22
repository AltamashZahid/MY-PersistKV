#include <set>

#include "pkql/plan.h"

namespace pkql {

namespace {

LPtr makeNode(LKind kind, LPtr child = nullptr) {
  auto n = std::make_shared<LNode>();
  n->kind = kind;
  if (child) {
    n->schema = child->schema;
    n->kids.push_back(std::move(child));
  }
  return n;
}

ExprPtr bindPredicate(const ExprPtr& e, const Schema& schema, const char* where) {
  ExprPtr b = foldConstants(bindExpr(e, schema));
  if (b->type != Type::Bool && b->type != Type::Null) {
    throw Error(std::string(where) + " needs a true/false expression, but `" + toString(*e) + "` is " +
                typeName(b->type));
  }
  return b;
}

// Gives every output column of `n` the qualifier `alias`, so that
// `(from ...) as x` and let-bound pipelines can be referenced as x.col.
LPtr requalify(LPtr n, const std::string& alias) {
  auto p = makeNode(LKind::Project, n);
  for (size_t i = 0; i < n->schema.size(); i++) {
    auto c = Expr::column(n->schema[i].qualifier, n->schema[i].name);
    c->index = static_cast<int>(i);
    c->type = n->schema[i].type;
    c->nullable = n->schema[i].nullable;
    p->exprs.push_back(c);
    p->schema[i].qualifier = alias;
  }
  return p;
}

ColumnInfo outputColumn(const NamedExpr& item, const ExprPtr& bound) {
  ColumnInfo c;
  c.type = bound->type;
  c.nullable = bound->nullable;
  if (!item.name.empty()) {
    c.name = item.name;
  } else if (bound->kind == ExprKind::Column) {
    c.name = bound->name;
    c.qualifier = bound->qualifier;
  } else {
    throw Error("give the computed column `" + toString(*item.expr) + "` a name, e.g. `x = " +
                toString(*item.expr) + "`");
  }
  return c;
}

class LogicalPlanner {
 public:
  LogicalPlanner(Catalog& catalog, const Lets& lets) : catalog_(catalog), lets_(lets) {}

  LPtr pipeline(const Pipeline& p) {
    LPtr n = source(p.from);
    for (const Step& s : p.steps) n = step(n, s);
    return n;
  }

 private:
  LPtr source(const Source& src) {
    if (src.sub) {
      LPtr n = pipeline(*src.sub);
      return src.alias.empty() ? n : requalify(n, src.alias);
    }
    if (const TableSchema* t = catalog_.find(src.name)) {
      auto n = makeNode(LKind::Scan);
      n->table = t;
      std::string q = src.alias.empty() ? t->name : src.alias;
      for (const Column& c : t->cols) n->schema.push_back({q, c.name, c.type, c.nullable});
      return n;
    }
    auto it = lets_.find(src.name);
    if (it != lets_.end()) {
      if (!expanding_.insert(src.name).second) throw Error("`" + src.name + "` refers to itself");
      LPtr n = pipeline(*it->second);
      expanding_.erase(src.name);
      return requalify(n, src.alias.empty() ? src.name : src.alias);
    }
    throw Error("no table or pipeline named `" + src.name + "`");
  }

  LPtr step(LPtr in, const Step& s) {
    switch (s.kind) {
      case StepKind::Filter: {
        auto n = makeNode(LKind::Filter, in);
        n->pred = bindPredicate(s.expr, in->schema, "filter");
        return n;
      }
      case StepKind::Select:
      case StepKind::Derive: {
        auto n = makeNode(LKind::Project, in);
        n->schema.clear();
        if (s.kind == StepKind::Derive) {
          for (size_t i = 0; i < in->schema.size(); i++) {
            auto c = Expr::column(in->schema[i].qualifier, in->schema[i].name);
            c->index = static_cast<int>(i);
            c->type = in->schema[i].type;
            c->nullable = in->schema[i].nullable;
            n->exprs.push_back(c);
            n->schema.push_back(in->schema[i]);
          }
        }
        for (const NamedExpr& item : s.items) {
          ExprPtr b = foldConstants(bindExpr(item.expr, in->schema));
          n->exprs.push_back(b);
          n->schema.push_back(outputColumn(item, b));
        }
        return n;
      }
      case StepKind::Join: {
        LPtr right = source(s.source);
        if (s.source.sub && s.source.alias.empty()) throw Error("a joined sub-pipeline needs an alias: `(...) as x`");
        auto n = makeNode(LKind::Join);
        n->left_join = s.left;
        n->kids = {in, right};
        n->schema = in->schema;
        for (ColumnInfo c : right->schema) {
          if (s.left) c.nullable = true;  // unmatched left rows get nulls on the right
          n->schema.push_back(c);
        }
        n->pred = bindPredicate(s.expr, n->schema, "join ... on");
        return n;
      }
      case StepKind::Group: {
        auto n = makeNode(LKind::Aggregate, in);
        n->schema.clear();
        for (const NamedExpr& key : s.items) {
          ExprPtr b = foldConstants(bindExpr(key.expr, in->schema));
          n->exprs.push_back(b);
          n->schema.push_back(outputColumn(key, b));
        }
        for (const AggItem& a : s.aggs) {
          AggSpec spec;
          spec.fn = a.fn;
          ColumnInfo c;
          c.name = a.name;
          if (a.arg) spec.arg = foldConstants(bindExpr(a.arg, in->schema));
          Type t = spec.arg ? spec.arg->type : Type::Null;
          if (a.fn == "count" || a.fn == "count_distinct") {
            c.type = Type::Int;
            c.nullable = false;
          } else if (a.fn == "avg") {
            if (t != Type::Int && t != Type::Float) throw Error("avg() needs a number");
            c.type = Type::Float;
          } else if (a.fn == "sum") {
            if (t != Type::Int && t != Type::Float) throw Error("sum() needs a number");
            c.type = t;
          } else {  // min, max
            c.type = t;
          }
          // An aggregate over a group is null only if every input is null, or
          // if `aggregate` runs over zero rows.
          if (a.fn != "count" && a.fn != "count_distinct") c.nullable = true;
          n->aggs.push_back(spec);
          n->schema.push_back(c);
        }
        return n;
      }
      case StepKind::Sort: {
        auto n = makeNode(LKind::Sort, in);
        for (const SortItem& it : s.sort) n->sort.push_back({foldConstants(bindExpr(it.expr, in->schema)), it.desc});
        return n;
      }
      case StepKind::Take: {
        if (s.n < 0) throw Error("take needs a non-negative count");
        if (in->kind == LKind::Limit && in->limit < 0) {  // skip n | take m
          in->limit = s.n;
          return in;
        }
        auto n = makeNode(LKind::Limit, in);
        n->limit = s.n;
        return n;
      }
      case StepKind::Skip: {
        if (s.n < 0) throw Error("skip needs a non-negative count");
        auto n = makeNode(LKind::Limit, in);
        n->offset = s.n;
        return n;
      }
      case StepKind::Distinct:
        return makeNode(LKind::Distinct, in);
      case StepKind::Delete:
      case StepKind::Update:
        throw Error("`delete` and `update` must be the last step, directly after `from table | filter ...`");
    }
    throw Error("unknown step");
  }

  Catalog& catalog_;
  const Lets& lets_;
  std::set<std::string> expanding_;
};

}  // namespace

LPtr buildLogicalPlan(const Pipeline& p, Catalog& catalog, const Lets& lets) {
  return LogicalPlanner(catalog, lets).pipeline(p);
}

}  // namespace pkql
