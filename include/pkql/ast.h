#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pkql/expr.h"

namespace pkql {

struct Pipeline;

// `from users`, `from adults` (a let-bound pipeline), or `(from ...) as x`.
struct Source {
  std::string name;
  std::shared_ptr<Pipeline> sub;
  std::string alias;
};

struct NamedExpr {
  std::string name;  // empty = derive from the expression
  ExprPtr expr;
};

struct AggItem {
  std::string name;
  std::string fn;  // count, sum, avg, min, max, count_distinct
  ExprPtr arg;     // nullptr for count()
};

struct SortItem {
  ExprPtr expr;
  bool desc = false;
};

enum class StepKind { Filter, Select, Derive, Join, Group, Sort, Take, Skip, Distinct, Delete, Update };

struct Step {
  StepKind kind = StepKind::Filter;
  ExprPtr expr;                  // filter predicate, join condition
  std::vector<NamedExpr> items;  // select / derive / update / group keys
  Source source;                 // join
  bool left = false;             // left join
  std::vector<AggItem> aggs;     // group
  std::vector<SortItem> sort;
  int64_t n = 0;                 // take / skip
};

// A query: a source followed by steps, applied top to bottom.
struct Pipeline {
  Source from;
  std::vector<Step> steps;
};

struct ColumnDef {
  std::string name;
  Type type = Type::Int;
  bool nullable = false;
  bool key = false;
};

enum class StmtKind {
  CreateTable, CreateIndex, Insert, Query, Let, Analyze, DropTable, DropIndex, ShowTables, Describe,
};

struct Statement {
  StmtKind kind = StmtKind::Query;
  std::string name;   // table / index / let name
  std::string table;  // index target
  std::vector<ColumnDef> cols;
  std::vector<std::string> index_cols;
  std::vector<std::vector<NamedExpr>> rows;  // insert
  std::shared_ptr<Pipeline> pipeline;
  bool explain = false;
  bool explain_analyze = false;

  // Plan cache: the statement's tokens with parameterizable literals
  // replaced by placeholders, and the values of those literals.
  std::string fingerprint;
  std::vector<Value> params;

  // For the lexer-only fast path: the statement's skeleton (all literals as
  // placeholders), the value of every literal token in order, and for each
  // parameter which literal it came from and whether it was negated (-5).
  std::string skeleton;
  std::vector<Value> literals;
  std::vector<int> param_literal;
  std::vector<bool> param_negated;
};

// Lexes a single statement without parsing it: its skeleton and literal
// values, in the same form parse() records them. False if the text is not
// exactly one statement.
bool skeletonOf(std::string_view text, std::string* skeleton, std::vector<Value>* literals);

// Parses a script of one or more statements, separated by `;` or by line
// breaks (a line starting with `|` continues the previous pipeline).
std::vector<Statement> parse(std::string_view text);

}  // namespace pkql
