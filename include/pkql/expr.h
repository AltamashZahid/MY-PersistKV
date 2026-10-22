#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pkql/value.h"

namespace pkql {

enum class ExprKind { Literal, Column, Unary, Binary, Call, InList };

enum class Op {
  Add, Sub, Mul, Div, IntDiv, Mod,
  Eq, Ne, Lt, Le, Gt, Ge,
  And, Or, Not, Neg,
  Coalesce,
};

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

// One node of an expression tree. The parser fills in names; binding
// resolves column names to positions in the input row and computes types.
struct Expr {
  ExprKind kind = ExprKind::Literal;
  Value value;                // Literal
  std::string qualifier;      // Column: table or alias, may be empty
  std::string name;           // Column name, or Call function name
  int index = -1;             // Column: position in the input row (bound)
  Op op = Op::Add;            // Unary / Binary
  std::vector<ExprPtr> args;  // operands; InList: args[0] is the tested value
  Type type = Type::Null;     // result type (bound)
  bool nullable = true;       // can evaluate to null (bound)

  static ExprPtr literal(Value v);
  static ExprPtr column(std::string qualifier, std::string name);
  static ExprPtr unary(Op op, ExprPtr a);
  static ExprPtr binary(Op op, ExprPtr a, ExprPtr b);
  static ExprPtr call(std::string fn, std::vector<ExprPtr> args);
};

// Output column of a plan node.
struct ColumnInfo {
  std::string qualifier;  // table or alias it came from ("" for computed columns)
  std::string name;
  Type type = Type::Null;
  bool nullable = true;
};
using Schema = std::vector<ColumnInfo>;

std::string displayName(const ColumnInfo& c);

// Resolves names against `schema` and type-checks. Throws Error with a
// message that names the problem (unknown column, ambiguous name, a text
// compared with a number, ...).
ExprPtr bindExpr(const ExprPtr& e, const Schema& schema);
int resolveColumn(const Schema& schema, const std::string& qualifier, const std::string& name);

Value eval(const Expr& e, const Row& row);
// Like eval, but columns and literals are returned by reference (no copy).
const Value& evalRef(const Expr& e, const Row& row, Value& scratch);
inline bool isTrue(const Value& v) { return v.type() == Type::Bool && v.asBool(); }

std::string toString(const Expr& e);  // for explain output and error messages

// Helpers used by the optimizer.
void splitConjuncts(const ExprPtr& e, std::vector<ExprPtr>& out);
ExprPtr makeConjunction(const std::vector<ExprPtr>& parts);  // nullptr if empty
void collectColumns(const Expr& e, std::vector<int>& out);
bool usesOnlyRange(const Expr& e, int lo, int hi);  // every column index in [lo, hi)
ExprPtr shiftColumns(const ExprPtr& e, int delta);
ExprPtr remapColumns(const ExprPtr& e, const std::vector<int>& mapping);  // index -> mapping[index]
ExprPtr substituteColumns(const ExprPtr& e, const std::vector<ExprPtr>& replacement);
ExprPtr foldConstants(const ExprPtr& e);
bool isConstant(const Expr& e);

}  // namespace pkql
