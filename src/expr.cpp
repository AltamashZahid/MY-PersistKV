#include "pkql/expr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace pkql {

ExprPtr Expr::literal(Value v) {
  auto e = std::make_shared<Expr>();
  e->kind = ExprKind::Literal;
  e->type = v.type();
  e->nullable = v.isNull();
  e->value = std::move(v);
  return e;
}

ExprPtr Expr::column(std::string qualifier, std::string name) {
  auto e = std::make_shared<Expr>();
  e->kind = ExprKind::Column;
  e->qualifier = std::move(qualifier);
  e->name = std::move(name);
  return e;
}

ExprPtr Expr::unary(Op op, ExprPtr a) {
  auto e = std::make_shared<Expr>();
  e->kind = ExprKind::Unary;
  e->op = op;
  e->args.push_back(std::move(a));
  return e;
}

ExprPtr Expr::binary(Op op, ExprPtr a, ExprPtr b) {
  auto e = std::make_shared<Expr>();
  e->kind = ExprKind::Binary;
  e->op = op;
  e->args.push_back(std::move(a));
  e->args.push_back(std::move(b));
  return e;
}

ExprPtr Expr::call(std::string fn, std::vector<ExprPtr> args) {
  auto e = std::make_shared<Expr>();
  e->kind = ExprKind::Call;
  e->name = std::move(fn);
  e->args = std::move(args);
  return e;
}

std::string displayName(const ColumnInfo& c) {
  return c.qualifier.empty() ? c.name : c.qualifier + "." + c.name;
}

// ---------------------------------------------------------------------------
// Binding and type checking
// ---------------------------------------------------------------------------

namespace {

bool isAggregateName(const std::string& n) {
  return n == "count" || n == "sum" || n == "avg" || n == "min" || n == "max" || n == "count_distinct";
}

bool numeric(Type t) { return t == Type::Int || t == Type::Float; }

bool compatible(Type a, Type b) {
  return a == b || a == Type::Null || b == Type::Null || (numeric(a) && numeric(b));
}

const char* opSymbol(Op op) {
  switch (op) {
    case Op::Add: return "+";
    case Op::Sub: return "-";
    case Op::Mul: return "*";
    case Op::Div: return "/";
    case Op::IntDiv: return "//";
    case Op::Mod: return "%";
    case Op::Eq: return "==";
    case Op::Ne: return "!=";
    case Op::Lt: return "<";
    case Op::Le: return "<=";
    case Op::Gt: return ">";
    case Op::Ge: return ">=";
    case Op::And: return "and";
    case Op::Or: return "or";
    case Op::Not: return "not";
    case Op::Neg: return "-";
    case Op::Coalesce: return "??";
  }
  return "?";
}

[[noreturn]] void typeError(const Expr& e, const std::string& why) {
  throw Error("type error in `" + toString(e) + "`: " + why);
}

void requireBool(const Expr& whole, const Expr& operand) {
  if (operand.type != Type::Bool && operand.type != Type::Null) {
    typeError(whole, std::string("`") + opSymbol(whole.op) + "` needs true/false operands, got " +
                         typeName(operand.type));
  }
}

}  // namespace

int resolveColumn(const Schema& schema, const std::string& qualifier, const std::string& name) {
  int found = -1;
  for (size_t i = 0; i < schema.size(); i++) {
    if (schema[i].name != name) continue;
    if (!qualifier.empty() && schema[i].qualifier != qualifier) continue;
    if (found >= 0) {
      throw Error("ambiguous column `" + name + "`: write " + displayName(schema[found]) + " or " +
                  displayName(schema[i]));
    }
    found = static_cast<int>(i);
  }
  if (found < 0) {
    std::string avail;
    for (const ColumnInfo& c : schema) avail += (avail.empty() ? "" : ", ") + displayName(c);
    throw Error("unknown column `" + (qualifier.empty() ? name : qualifier + "." + name) + "` (available: " +
                avail + ")");
  }
  return found;
}

ExprPtr bindExpr(const ExprPtr& src, const Schema& schema) {
  auto e = std::make_shared<Expr>(*src);
  for (ExprPtr& a : e->args) a = bindExpr(a, schema);

  switch (e->kind) {
    case ExprKind::Literal:
      break;

    case ExprKind::Column: {
      e->index = resolveColumn(schema, e->qualifier, e->name);
      const ColumnInfo& c = schema[e->index];
      e->qualifier = c.qualifier;
      e->type = c.type;
      e->nullable = c.nullable;
      break;
    }

    case ExprKind::Unary: {
      const Expr& a = *e->args[0];
      if (e->op == Op::Not) {
        requireBool(*e, a);
        e->type = Type::Bool;
        e->nullable = false;
      } else {
        if (!numeric(a.type) && a.type != Type::Null) typeError(*e, "negation needs a number");
        e->type = a.type;
        e->nullable = a.nullable;
      }
      break;
    }

    case ExprKind::Binary: {
      const Expr& a = *e->args[0];
      const Expr& b = *e->args[1];
      switch (e->op) {
        case Op::And:
        case Op::Or:
          requireBool(*e, a);
          requireBool(*e, b);
          e->type = Type::Bool;
          e->nullable = false;
          break;
        case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
          if (!compatible(a.type, b.type)) {
            typeError(*e, std::string("cannot compare ") + typeName(a.type) + " with " + typeName(b.type));
          }
          e->type = Type::Bool;
          e->nullable = false;  // comparisons are always true or false, never null
          break;
        case Op::Coalesce:
          if (!compatible(a.type, b.type)) {
            typeError(*e, std::string("both sides of ?? must have the same type, got ") + typeName(a.type) +
                              " and " + typeName(b.type));
          }
          e->type = a.type != Type::Null ? a.type : b.type;
          if (numeric(a.type) && numeric(b.type) && a.type != b.type) e->type = Type::Float;
          e->nullable = a.nullable && b.nullable;
          break;
        default: {  // arithmetic
          bool texts = a.type == Type::Text && b.type == Type::Text;
          if (texts && e->op == Op::Add) {
            e->type = Type::Text;
          } else {
            if (!(numeric(a.type) || a.type == Type::Null) || !(numeric(b.type) || b.type == Type::Null)) {
              typeError(*e, std::string("`") + opSymbol(e->op) + "` needs numbers, got " + typeName(a.type) +
                                " and " + typeName(b.type));
            }
            if (e->op == Op::Div) {
              e->type = Type::Float;  // 7 / 2 is 3.5; use // for integer division
            } else if (e->op == Op::IntDiv || e->op == Op::Mod) {
              if (a.type == Type::Float || b.type == Type::Float) typeError(*e, "`//` and `%` need integers");
              e->type = Type::Int;
            } else {
              e->type = (a.type == Type::Float || b.type == Type::Float) ? Type::Float : Type::Int;
            }
          }
          // Division by zero yields null, so / // % are always nullable.
          e->nullable = a.nullable || b.nullable || e->op == Op::Div || e->op == Op::IntDiv || e->op == Op::Mod;
          break;
        }
      }
      break;
    }

    case ExprKind::InList: {
      const Expr& a = *e->args[0];
      for (size_t i = 1; i < e->args.size(); i++) {
        if (!compatible(a.type, e->args[i]->type)) {
          typeError(*e, std::string("list value has type ") + typeName(e->args[i]->type) + ", expected " +
                            typeName(a.type));
        }
      }
      e->type = Type::Bool;
      e->nullable = false;
      break;
    }

    case ExprKind::Call: {
      const std::string& fn = e->name;
      auto argc = [&](size_t n) {
        if (e->args.size() != n) {
          throw Error("function " + fn + " takes " + std::to_string(n) + " argument" + (n == 1 ? "" : "s"));
        }
      };
      auto textArg = [&](size_t i) {
        if (e->args[i]->type != Type::Text && e->args[i]->type != Type::Null) {
          typeError(*e, fn + " needs text, got " + typeName(e->args[i]->type));
        }
      };
      if (isAggregateName(fn)) {
        throw Error("aggregate " + fn + "() can only be used inside group (...) or aggregate (...)");
      } else if (fn == "lower" || fn == "upper") {
        argc(1);
        textArg(0);
        e->type = Type::Text;
        e->nullable = e->args[0]->nullable;
      } else if (fn == "length") {
        argc(1);
        textArg(0);
        e->type = Type::Int;
        e->nullable = e->args[0]->nullable;
      } else if (fn == "starts_with" || fn == "contains") {
        argc(2);
        textArg(0);
        textArg(1);
        e->type = Type::Bool;
        e->nullable = false;
      } else if (fn == "abs") {
        argc(1);
        if (!numeric(e->args[0]->type)) typeError(*e, "abs needs a number");
        e->type = e->args[0]->type;
        e->nullable = e->args[0]->nullable;
      } else if (fn == "round") {
        argc(1);
        if (!numeric(e->args[0]->type)) typeError(*e, "round needs a number");
        e->type = Type::Int;
        e->nullable = e->args[0]->nullable;
      } else {
        throw Error("unknown function " + fn + "()");
      }
      break;
    }
  }
  return e;
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

namespace {

Value arith(Op op, const Value& a, const Value& b) {
  if (a.isNull() || b.isNull()) return Value::null();
  if (op == Op::Add && a.type() == Type::Text) return Value::text(a.asText() + b.asText());
  if (op == Op::Div) {
    double d = b.asNumber();
    if (d == 0) return Value::null();
    return Value::real(a.asNumber() / d);
  }
  if (a.type() == Type::Int && b.type() == Type::Int) {
    int64_t x = a.asInt(), y = b.asInt();
    switch (op) {
      case Op::Add: return Value::integer(x + y);
      case Op::Sub: return Value::integer(x - y);
      case Op::Mul: return Value::integer(x * y);
      case Op::IntDiv: return y == 0 ? Value::null() : Value::integer(x / y);
      case Op::Mod: return y == 0 ? Value::null() : Value::integer(x % y);
      default: break;
    }
  }
  double x = a.asNumber(), y = b.asNumber();
  switch (op) {
    case Op::Add: return Value::real(x + y);
    case Op::Sub: return Value::real(x - y);
    case Op::Mul: return Value::real(x * y);
    default: break;
  }
  throw Error("bad arithmetic");
}

}  // namespace

// Columns and literals are returned by reference, so comparing a text
// column against a constant copies no strings.
const Value& evalRef(const Expr& e, const Row& row, Value& scratch) {
  if (e.kind == ExprKind::Column) return row[e.index];
  if (e.kind == ExprKind::Literal) return e.value;
  scratch = eval(e, row);
  return scratch;
}

Value eval(const Expr& e, const Row& row) {
  switch (e.kind) {
    case ExprKind::Literal: return e.value;
    case ExprKind::Column: return row[e.index];

    case ExprKind::Unary: {
      Value t;
      const Value& a = evalRef(*e.args[0], row, t);
      if (e.op == Op::Not) return Value::boolean(!isTrue(a));
      if (a.isNull()) return a;
      return a.type() == Type::Int ? Value::integer(-a.asInt()) : Value::real(-a.asFloat());
    }

    case ExprKind::Binary: {
      Value ta, tb;
      switch (e.op) {
        case Op::And:
          return Value::boolean(isTrue(evalRef(*e.args[0], row, ta)) && isTrue(evalRef(*e.args[1], row, tb)));
        case Op::Or:
          return Value::boolean(isTrue(evalRef(*e.args[0], row, ta)) || isTrue(evalRef(*e.args[1], row, tb)));
        case Op::Coalesce: {
          const Value& a = evalRef(*e.args[0], row, ta);
          return a.isNull() ? evalRef(*e.args[1], row, tb) : a;
        }
        default: break;
      }
      const Value& a = evalRef(*e.args[0], row, ta);
      const Value& b = evalRef(*e.args[1], row, tb);
      switch (e.op) {
        case Op::Eq: return Value::boolean(compare(a, b) == 0);
        case Op::Ne: return Value::boolean(compare(a, b) != 0);
        case Op::Lt: return Value::boolean(compare(a, b) < 0);
        case Op::Le: return Value::boolean(compare(a, b) <= 0);
        case Op::Gt: return Value::boolean(compare(a, b) > 0);
        case Op::Ge: return Value::boolean(compare(a, b) >= 0);
        default: return arith(e.op, a, b);
      }
    }

    case ExprKind::InList: {
      Value ta, tb;
      const Value& a = evalRef(*e.args[0], row, ta);
      for (size_t i = 1; i < e.args.size(); i++) {
        if (compare(a, evalRef(*e.args[i], row, tb)) == 0) return Value::boolean(true);
      }
      return Value::boolean(false);
    }

    case ExprKind::Call: {
      const std::string& fn = e.name;
      Value t0, t1;
      const Value& a = evalRef(*e.args[0], row, t0);
      if (fn == "starts_with" || fn == "contains") {
        const Value& b = evalRef(*e.args[1], row, t1);
        if (a.isNull() || b.isNull()) return Value::boolean(false);
        const std::string& s = a.asText();
        const std::string& p = b.asText();
        bool r = fn == "contains" ? s.find(p) != std::string::npos : s.compare(0, p.size(), p) == 0;
        return Value::boolean(r);
      }
      if (a.isNull()) return a;
      if (fn == "lower" || fn == "upper") {
        std::string s = a.asText();
        for (char& c : s) c = static_cast<char>(fn == "lower" ? std::tolower(static_cast<unsigned char>(c))
                                                               : std::toupper(static_cast<unsigned char>(c)));
        return Value::text(std::move(s));
      }
      if (fn == "length") return Value::integer(static_cast<int64_t>(a.asText().size()));
      if (fn == "abs") return a.type() == Type::Int ? Value::integer(std::llabs(a.asInt())) : Value::real(std::fabs(a.asFloat()));
      if (fn == "round") return Value::integer(static_cast<int64_t>(std::llround(a.asNumber())));
      throw Error("unknown function " + fn);
    }
  }
  return Value::null();
}

// ---------------------------------------------------------------------------
// Printing and tree helpers
// ---------------------------------------------------------------------------

std::string toString(const Expr& e) {
  switch (e.kind) {
    case ExprKind::Literal: return e.value.literal();
    case ExprKind::Column: return e.qualifier.empty() ? e.name : e.qualifier + "." + e.name;
    case ExprKind::Unary:
      return e.op == Op::Not ? "not " + toString(*e.args[0]) : "-" + toString(*e.args[0]);
    case ExprKind::Binary: {
      std::string s = toString(*e.args[0]) + " " + opSymbol(e.op) + " " + toString(*e.args[1]);
      return (e.op == Op::And || e.op == Op::Or) ? "(" + s + ")" : s;
    }
    case ExprKind::InList: {
      std::string s = toString(*e.args[0]) + " in [";
      for (size_t i = 1; i < e.args.size(); i++) s += (i > 1 ? ", " : "") + toString(*e.args[i]);
      return s + "]";
    }
    case ExprKind::Call: {
      std::string s = e.name + "(";
      for (size_t i = 0; i < e.args.size(); i++) s += (i ? ", " : "") + toString(*e.args[i]);
      return s + ")";
    }
  }
  return "?";
}

void splitConjuncts(const ExprPtr& e, std::vector<ExprPtr>& out) {
  if (!e) return;
  if (e->kind == ExprKind::Binary && e->op == Op::And) {
    splitConjuncts(e->args[0], out);
    splitConjuncts(e->args[1], out);
  } else {
    out.push_back(e);
  }
}

ExprPtr makeConjunction(const std::vector<ExprPtr>& parts) {
  ExprPtr acc;
  for (const ExprPtr& p : parts) {
    if (!acc) {
      acc = p;
    } else {
      acc = Expr::binary(Op::And, acc, p);
      acc->type = Type::Bool;
      acc->nullable = false;
    }
  }
  return acc;
}

void collectColumns(const Expr& e, std::vector<int>& out) {
  if (e.kind == ExprKind::Column) out.push_back(e.index);
  for (const ExprPtr& a : e.args) collectColumns(*a, out);
}

bool usesOnlyRange(const Expr& e, int lo, int hi) {
  std::vector<int> cols;
  collectColumns(e, cols);
  return std::all_of(cols.begin(), cols.end(), [&](int c) { return c >= lo && c < hi; });
}

ExprPtr remapColumns(const ExprPtr& e, const std::vector<int>& mapping) {
  auto out = std::make_shared<Expr>(*e);
  if (out->kind == ExprKind::Column) {
    out->index = mapping[out->index];
    if (out->index < 0) throw Error("internal error: column " + out->name + " lost during planning");
  }
  for (ExprPtr& a : out->args) a = remapColumns(a, mapping);
  return out;
}

ExprPtr shiftColumns(const ExprPtr& e, int delta) {
  auto out = std::make_shared<Expr>(*e);
  if (out->kind == ExprKind::Column) out->index += delta;
  for (ExprPtr& a : out->args) a = shiftColumns(a, delta);
  return out;
}

ExprPtr substituteColumns(const ExprPtr& e, const std::vector<ExprPtr>& replacement) {
  if (e->kind == ExprKind::Column) return replacement[e->index];
  auto out = std::make_shared<Expr>(*e);
  for (ExprPtr& a : out->args) a = substituteColumns(a, replacement);
  return out;
}

bool isConstant(const Expr& e) {
  if (e.kind == ExprKind::Column) return false;
  return std::all_of(e.args.begin(), e.args.end(), [](const ExprPtr& a) { return isConstant(*a); });
}

ExprPtr foldConstants(const ExprPtr& e) {
  if (e->kind == ExprKind::Literal) return e;
  if (isConstant(*e)) {
    ExprPtr lit = Expr::literal(eval(*e, Row()));
    if (e->type == Type::Float) lit->value = lit->value.castTo(Type::Float);
    lit->type = lit->value.isNull() ? e->type : lit->value.type();
    return lit;
  }
  auto out = std::make_shared<Expr>(*e);
  for (ExprPtr& a : out->args) a = foldConstants(a);
  return out;
}

}  // namespace pkql
