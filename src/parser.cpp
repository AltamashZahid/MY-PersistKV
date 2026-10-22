#include <cctype>
#include <cstdlib>

#include "pkql/ast.h"

namespace pkql {

namespace {

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

enum class Tok { Ident, Int, Float, String, Sym, Semi, End };

struct Token {
  Tok kind;
  std::string text;
  int line, col;
};

bool isContinuation(const Token& t) {
  // A line ending in one of these continues on the next line.
  static const char* syms[] = {"|", ",", "(", "[", "{", "=", "==", "!=", "<", "<=", ">", ">=", "+", "-",
                               "*", "/", "//", "%", "??", ":", "."};
  if (t.kind == Tok::Ident) return t.text == "and" || t.text == "or" || t.text == "not" || t.text == "in";
  if (t.kind != Tok::Sym) return false;
  for (const char* s : syms) {
    if (t.text == s) return true;
  }
  return false;
}

std::vector<Token> lex(std::string_view src) {
  std::vector<Token> toks;
  int line = 1, col = 1, depth = 0;
  size_t i = 0;
  auto error = [&](const std::string& why) -> Error {
    return Error("line " + std::to_string(line) + ", col " + std::to_string(col) + ": " + why);
  };
  auto push = [&](Tok k, std::string text, int c) { toks.push_back({k, std::move(text), line, c}); };

  while (i < src.size()) {
    char c = src[i];
    if (c == '#') {
      while (i < src.size() && src[i] != '\n') i++;
      continue;
    }
    if (c == '\n') {
      // A line break ends a statement unless we are inside brackets or the
      // last token on the line asks for more.
      if (depth == 0 && !toks.empty() && toks.back().kind != Tok::Semi && !isContinuation(toks.back())) {
        push(Tok::Semi, "\\n", col);
      }
      i++;
      line++;
      col = 1;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      i++;
      col++;
      continue;
    }
    int start_col = col;
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      size_t j = i;
      while (j < src.size() && (std::isalnum(static_cast<unsigned char>(src[j])) || src[j] == '_')) j++;
      push(Tok::Ident, std::string(src.substr(i, j - i)), start_col);
      col += static_cast<int>(j - i);
      i = j;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      size_t j = i;
      bool is_float = false;
      while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j]))) j++;
      if (j + 1 < src.size() && src[j] == '.' && std::isdigit(static_cast<unsigned char>(src[j + 1]))) {
        is_float = true;
        j++;
        while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j]))) j++;
      }
      if (j < src.size() && (src[j] == 'e' || src[j] == 'E')) {
        size_t k = j + 1;
        if (k < src.size() && (src[k] == '+' || src[k] == '-')) k++;
        if (k < src.size() && std::isdigit(static_cast<unsigned char>(src[k]))) {
          is_float = true;
          j = k;
          while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j]))) j++;
        }
      }
      push(is_float ? Tok::Float : Tok::Int, std::string(src.substr(i, j - i)), start_col);
      col += static_cast<int>(j - i);
      i = j;
      continue;
    }
    if (c == '"' || c == '\'') {
      std::string s;
      size_t j = i + 1;
      while (true) {
        if (j >= src.size() || src[j] == '\n') throw error("unterminated string");
        if (src[j] == c) break;
        if (src[j] == '\\' && j + 1 < src.size()) {
          char e = src[j + 1];
          s += e == 'n' ? '\n' : e == 't' ? '\t' : e;
          j += 2;
          continue;
        }
        s += src[j++];
      }
      push(Tok::String, s, start_col);
      col += static_cast<int>(j + 1 - i);
      i = j + 1;
      continue;
    }
    if (c == ';') {
      if (!toks.empty() && toks.back().kind != Tok::Semi) push(Tok::Semi, ";", start_col);
      i++;
      col++;
      continue;
    }
    static const char* two[] = {"==", "!=", "<=", ">=", "//", "??"};
    std::string sym;
    for (const char* t : two) {
      if (src.compare(i, 2, t) == 0) sym = t;
    }
    if (sym.empty()) {
      if (std::string("|,(){}[]:?.=<>+-*/%").find(c) == std::string::npos) {
        throw error(std::string("unexpected character '") + c + "'");
      }
      sym = std::string(1, c);
    }
    if (sym == "(" || sym == "[" || sym == "{") depth++;
    if (sym == ")" || sym == "]" || sym == "}") depth = depth > 0 ? depth - 1 : 0;
    // A line starting with `|` continues the previous pipeline.
    if (sym == "|" && !toks.empty() && toks.back().kind == Tok::Semi && toks.back().text == "\\n") toks.pop_back();
    push(Tok::Sym, sym, start_col);
    col += static_cast<int>(sym.size());
    i += sym.size();
  }
  toks.push_back({Tok::End, "", line, col});
  return toks;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

const char* kAggregates[] = {"count", "sum", "avg", "min", "max", "count_distinct"};

bool isAggregate(const std::string& s) {
  for (const char* a : kAggregates) {
    if (s == a) return true;
  }
  return false;
}

class Parser {
 public:
  explicit Parser(std::vector<Token> toks) : t_(std::move(toks)) {}

  std::vector<Statement> script() {
    std::vector<Statement> out;
    while (true) {
      while (peek().kind == Tok::Semi) p_++;
      if (peek().kind == Tok::End) break;
      out.push_back(statement());
      if (peek().kind != Tok::Semi && peek().kind != Tok::End) fail("expected the end of the statement");
    }
    return out;
  }

 private:
  const Token& peek(size_t k = 0) const { return t_[std::min(p_ + k, t_.size() - 1)]; }
  const Token& take() { return t_[p_ < t_.size() - 1 ? p_++ : p_]; }

  [[noreturn]] void fail(const std::string& why) const {
    const Token& t = peek();
    std::string near = t.kind == Tok::End ? "end of input" : t.kind == Tok::Semi ? "end of line" : "`" + t.text + "`";
    throw Error("line " + std::to_string(t.line) + ", col " + std::to_string(t.col) + ": " + why + " (near " +
                near + ")");
  }

  bool isWord(const char* w, size_t k = 0) const { return peek(k).kind == Tok::Ident && peek(k).text == w; }
  bool isSym(const char* s, size_t k = 0) const { return peek(k).kind == Tok::Sym && peek(k).text == s; }

  bool acceptWord(const char* w) {
    if (!isWord(w)) return false;
    p_++;
    return true;
  }
  bool acceptSym(const char* s) {
    if (!isSym(s)) return false;
    p_++;
    return true;
  }
  void expectWord(const char* w) {
    if (!acceptWord(w)) fail(std::string("expected `") + w + "`");
  }
  void expectSym(const char* s) {
    if (!acceptSym(s)) fail(std::string("expected `") + s + "`");
  }
  std::string ident(const char* what) {
    if (peek().kind != Tok::Ident) fail(std::string("expected ") + what);
    return take().text;
  }
  int64_t integer(const char* what) {
    if (peek().kind != Tok::Int) fail(std::string("expected ") + what);
    return std::strtoll(take().text.c_str(), nullptr, 10);
  }

  Statement statement() {
    Statement s;
    if (acceptWord("table")) {
      s.kind = StmtKind::CreateTable;
      s.name = ident("a table name");
      expectSym("(");
      do {
        ColumnDef c;
        c.name = ident("a column name");
        expectSym(":");
        std::string ty = ident("a type (int, float, text, bool)");
        if (ty == "int") c.type = Type::Int;
        else if (ty == "float") c.type = Type::Float;
        else if (ty == "text") c.type = Type::Text;
        else if (ty == "bool") c.type = Type::Bool;
        else fail("unknown type `" + ty + "` (use int, float, text or bool)");
        c.nullable = acceptSym("?");
        c.key = acceptWord("key");
        s.cols.push_back(c);
      } while (acceptSym(","));
      expectSym(")");
    } else if (acceptWord("index")) {
      s.kind = StmtKind::CreateIndex;
      s.name = ident("an index name");
      expectWord("on");
      s.table = ident("a table name");
      expectSym("(");
      do s.index_cols.push_back(ident("a column name"));
      while (acceptSym(","));
      expectSym(")");
    } else if (acceptWord("insert")) {
      s.kind = StmtKind::Insert;
      s.name = ident("a table name");
      do {
        expectSym("{");
        std::vector<NamedExpr> row;
        if (!isSym("}")) {
          do {
            NamedExpr f;
            f.name = ident("a column name");
            expectSym(":");
            f.expr = expr();
            row.push_back(f);
          } while (acceptSym(","));
        }
        expectSym("}");
        s.rows.push_back(row);
      } while (acceptSym(","));
    } else if (acceptWord("let")) {
      s.kind = StmtKind::Let;
      s.name = ident("a name");
      expectSym("=");
      s.pipeline = pipeline();
    } else if (acceptWord("analyze")) {
      s.kind = StmtKind::Analyze;
      if (peek().kind == Tok::Ident) s.name = take().text;  // empty = every table
    } else if (acceptWord("drop")) {
      if (acceptWord("table")) s.kind = StmtKind::DropTable;
      else if (acceptWord("index")) s.kind = StmtKind::DropIndex;
      else fail("expected `table` or `index`");
      s.name = ident("a name");
    } else if (acceptWord("show")) {
      expectWord("tables");
      s.kind = StmtKind::ShowTables;
    } else if (acceptWord("describe")) {
      s.kind = StmtKind::Describe;
      s.name = ident("a table name");
    } else if (acceptWord("explain")) {
      s.kind = StmtKind::Query;
      s.explain = true;
      s.explain_analyze = acceptWord("analyze");
      s.pipeline = pipeline();
    } else if (isWord("from")) {
      s.kind = StmtKind::Query;
      s.pipeline = pipeline();
    } else {
      fail("expected a statement (from, table, index, insert, let, analyze, explain, drop, show, describe)");
    }
    return s;
  }

  Source source(bool require_alias_for_sub) {
    Source src;
    if (acceptSym("(")) {
      src.sub = pipeline();
      expectSym(")");
      if (require_alias_for_sub || isWord("as")) {
        expectWord("as");
        src.alias = ident("an alias");
      }
    } else {
      src.name = ident("a table or pipeline name");
      if (acceptWord("as")) src.alias = ident("an alias");
    }
    return src;
  }

  std::shared_ptr<Pipeline> pipeline() {
    auto p = std::make_shared<Pipeline>();
    expectWord("from");
    p->from = source(false);
    while (acceptSym("|")) p->steps.push_back(step());
    return p;
  }

  NamedExpr item(bool require_name) {
    NamedExpr n;
    if (peek().kind == Tok::Ident && isSym("=", 1)) {
      n.name = take().text;
      p_++;  // '='
    } else if (require_name) {
      fail("expected `name = expression`");
    }
    n.expr = expr();
    return n;
  }

  std::vector<NamedExpr> items(bool require_name) {
    std::vector<NamedExpr> out;
    do out.push_back(item(require_name));
    while (acceptSym(","));
    return out;
  }

  std::vector<AggItem> aggregates() {
    std::vector<AggItem> out;
    expectSym("{");
    do {
      AggItem a;
      if (peek().kind == Tok::Ident && isSym("=", 1)) {
        a.name = take().text;
        p_++;
      }
      a.fn = ident("an aggregate (count, sum, avg, min, max, count_distinct)");
      if (!isAggregate(a.fn)) fail("`" + a.fn + "` is not an aggregate (count, sum, avg, min, max, count_distinct)");
      expectSym("(");
      if (!isSym(")")) a.arg = expr();
      expectSym(")");
      if (!a.arg && a.fn != "count") fail(a.fn + "() needs an argument");
      if (a.name.empty()) a.name = a.arg ? a.fn + "_" + toString(*a.arg) : a.fn;
      for (char& c : a.name) {
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
      }
      out.push_back(a);
    } while (acceptSym(","));
    expectSym("}");
    return out;
  }

  Step step() {
    Step s;
    if (acceptWord("filter")) {
      s.kind = StepKind::Filter;
      s.expr = expr();
    } else if (acceptWord("select")) {
      s.kind = StepKind::Select;
      s.items = items(false);
    } else if (acceptWord("derive")) {
      s.kind = StepKind::Derive;
      s.items = items(true);
    } else if (isWord("join") || (isWord("left") && isWord("join", 1))) {
      s.kind = StepKind::Join;
      s.left = acceptWord("left");
      expectWord("join");
      s.source = source(true);
      expectWord("on");
      s.expr = expr();
    } else if (acceptWord("group")) {
      s.kind = StepKind::Group;
      if (!isSym("{")) s.items = items(false);
      s.aggs = aggregates();
    } else if (acceptWord("aggregate")) {
      s.kind = StepKind::Group;  // a group with no keys
      s.aggs = aggregates();
    } else if (acceptWord("sort")) {
      s.kind = StepKind::Sort;
      do {
        SortItem it;
        it.desc = acceptSym("-");
        it.expr = expr();
        s.sort.push_back(it);
      } while (acceptSym(","));
    } else if (acceptWord("take")) {
      s.kind = StepKind::Take;
      s.n = integer("a row count");
    } else if (acceptWord("skip")) {
      s.kind = StepKind::Skip;
      s.n = integer("a row count");
    } else if (acceptWord("distinct")) {
      s.kind = StepKind::Distinct;
    } else if (acceptWord("delete")) {
      s.kind = StepKind::Delete;
    } else if (acceptWord("update")) {
      s.kind = StepKind::Update;
      s.items = items(true);
    } else {
      fail("expected a step (filter, select, derive, join, group, aggregate, sort, take, skip, distinct, "
           "update, delete)");
    }
    return s;
  }

  // Expressions, lowest precedence first.
  ExprPtr expr() { return orExpr(); }

  ExprPtr orExpr() {
    ExprPtr e = andExpr();
    while (acceptWord("or")) e = Expr::binary(Op::Or, e, andExpr());
    return e;
  }
  ExprPtr andExpr() {
    ExprPtr e = notExpr();
    while (acceptWord("and")) e = Expr::binary(Op::And, e, notExpr());
    return e;
  }
  ExprPtr notExpr() {
    if (acceptWord("not")) return Expr::unary(Op::Not, notExpr());
    return comparison();
  }
  ExprPtr comparison() {
    ExprPtr e = coalesce();
    static const std::pair<const char*, Op> ops[] = {{"==", Op::Eq}, {"!=", Op::Ne}, {"<=", Op::Le},
                                                     {">=", Op::Ge}, {"<", Op::Lt},  {">", Op::Gt}};
    for (const auto& o : ops) {
      if (acceptSym(o.first)) return Expr::binary(o.second, e, coalesce());
    }
    if (isSym("=")) fail("use `==` to compare (`=` is only for naming columns)");
    if (acceptWord("in")) {
      auto in = std::make_shared<Expr>();
      in->kind = ExprKind::InList;
      in->args.push_back(e);
      expectSym("[");
      if (!isSym("]")) {
        do in->args.push_back(coalesce());
        while (acceptSym(","));
      }
      expectSym("]");
      return in;
    }
    return e;
  }
  ExprPtr coalesce() {
    ExprPtr e = additive();
    while (acceptSym("??")) e = Expr::binary(Op::Coalesce, e, additive());
    return e;
  }
  ExprPtr additive() {
    ExprPtr e = multiplicative();
    while (true) {
      if (acceptSym("+")) e = Expr::binary(Op::Add, e, multiplicative());
      else if (acceptSym("-")) e = Expr::binary(Op::Sub, e, multiplicative());
      else return e;
    }
  }
  ExprPtr multiplicative() {
    ExprPtr e = unary();
    while (true) {
      if (acceptSym("*")) e = Expr::binary(Op::Mul, e, unary());
      else if (acceptSym("//")) e = Expr::binary(Op::IntDiv, e, unary());
      else if (acceptSym("/")) e = Expr::binary(Op::Div, e, unary());
      else if (acceptSym("%")) e = Expr::binary(Op::Mod, e, unary());
      else return e;
    }
  }
  ExprPtr unary() {
    if (acceptSym("-")) {
      ExprPtr a = unary();
      if (a->kind == ExprKind::Literal && a->value.type() == Type::Int) return Expr::literal(Value::integer(-a->value.asInt()));
      if (a->kind == ExprKind::Literal && a->value.type() == Type::Float) return Expr::literal(Value::real(-a->value.asFloat()));
      return Expr::unary(Op::Neg, a);
    }
    return primary();
  }
  ExprPtr primary() {
    const Token& t = peek();
    switch (t.kind) {
      case Tok::Int: {
        errno = 0;
        long long v = std::strtoll(t.text.c_str(), nullptr, 10);
        if (errno == ERANGE) fail("integer out of range");
        p_++;
        return Expr::literal(Value::integer(v));
      }
      case Tok::Float:
        p_++;
        return Expr::literal(Value::real(std::strtod(t.text.c_str(), nullptr)));
      case Tok::String: {
        std::string s = t.text;
        p_++;
        return Expr::literal(Value::text(s));
      }
      case Tok::Ident: {
        std::string name = take().text;
        if (name == "true") return Expr::literal(Value::boolean(true));
        if (name == "false") return Expr::literal(Value::boolean(false));
        if (name == "null") return Expr::literal(Value::null());
        if (acceptSym("(")) {
          std::vector<ExprPtr> args;
          if (!isSym(")")) {
            do args.push_back(expr());
            while (acceptSym(","));
          }
          expectSym(")");
          return Expr::call(name, args);
        }
        if (acceptSym(".")) return Expr::column(name, ident("a column name"));
        return Expr::column("", name);
      }
      case Tok::Sym:
        if (acceptSym("(")) {
          ExprPtr e = expr();
          expectSym(")");
          return e;
        }
        break;
      default:
        break;
    }
    fail("expected a value, column or `(`");
  }

  std::vector<Token> t_;
  size_t p_ = 0;
};

}  // namespace

std::vector<Statement> parse(std::string_view text) { return Parser(lex(text)).script(); }

}  // namespace pkql
