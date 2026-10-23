// Differential test: the same random data goes into PKQL and SQLite, and the
// same randomly generated queries (written in both languages) must return
// the same rows, under several optimizer configurations.
//
//   differential_test [queries] [seed]

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "persistkv/file.h"
#include "pkql/engine.h"

using namespace pkql;

namespace {

struct Col {
  std::string name;
  Type type;
  std::vector<Value> domain;  // sample constants for predicates
};

struct Tab {
  std::string name;
  std::vector<Col> cols;
};

std::mt19937 rng;   // query shape
std::mt19937 lrng;  // literal values, so one shape can be re-run with other constants

int pick(int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); }

const char* kCities[] = {"Pune", "Delhi", "Mumbai", "Chennai", "Agra", "Goa"};
const char* kStatus[] = {"open", "paid", "shipped", "returned"};
const char* kCategory[] = {"books", "games", "garden", "tools", "toys"};

void exec(sqlite3* db, const std::string& sql) {
  char* err = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    std::string e = err ? err : "?";
    sqlite3_free(err);
    throw std::runtime_error("sqlite: " + e + "\n  in: " + sql);
  }
}

std::string sqlLiteral(const Value& v) {
  if (v.type() != Type::Text) return v.toString();
  std::string s = "'";
  for (char c : v.asText()) s += c == '\'' ? std::string("''") : std::string(1, c);
  return s + "'";
}

// Rows normalized for comparison: floats rounded so summation order does
// not matter.
std::string norm(const Value& v) {
  if (v.isNull()) return "null";
  if (v.isNumber()) {
    double d = v.asNumber();
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.4f", std::round(d * 1e4) / 1e4);
    return buf;
  }
  if (v.type() == Type::Bool) return v.asBool() ? "1.0000" : "0.0000";
  return "'" + v.asText() + "'";
}

using NormRows = std::vector<std::vector<std::string>>;

NormRows normalize(const std::vector<Row>& rows) {
  NormRows out;
  for (const Row& r : rows) {
    std::vector<std::string> n;
    for (const Value& v : r) n.push_back(norm(v));
    out.push_back(n);
  }
  return out;
}

NormRows runSqlite(sqlite3* db, const std::string& sql) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    throw std::runtime_error(std::string("sqlite prepare: ") + sqlite3_errmsg(db) + "\n  in: " + sql);
  }
  std::vector<Row> rows;
  while (sqlite3_step(st) == SQLITE_ROW) {
    Row r;
    for (int i = 0; i < sqlite3_column_count(st); i++) {
      switch (sqlite3_column_type(st, i)) {
        case SQLITE_INTEGER: r.push_back(Value::integer(sqlite3_column_int64(st, i))); break;
        case SQLITE_FLOAT: r.push_back(Value::real(sqlite3_column_double(st, i))); break;
        case SQLITE_TEXT: r.push_back(Value::text(reinterpret_cast<const char*>(sqlite3_column_text(st, i)))); break;
        default: r.push_back(Value::null()); break;
      }
    }
    rows.push_back(r);
  }
  sqlite3_finalize(st);
  return normalize(rows);
}

// --- Data -----------------------------------------------------------------------

std::vector<Tab> buildData(Engine& pk, sqlite3* sq) {
  pk.run(R"(
    table users (id: int key, name: text, city: text, age: int)
    table products (id: int key, category: text, price: float)
    table orders (id: int key, user_id: int, product_id: int, amount: float, status: text, qty: int)
    index users_city on users (city)
    index products_category on products (category)
    index orders_user on orders (user_id)
    index orders_product on orders (product_id)
  )");
  exec(sq, R"(
    create table users (id integer primary key, name text, city text, age integer);
    create table products (id integer primary key, category text, price real);
    create table orders (id integer primary key, user_id integer, product_id integer, amount real, status text, qty integer);
    create index users_city on users (city);
    create index products_category on products (category);
    create index orders_user on orders (user_id);
    create index orders_product on orders (product_id);
  )");

  const int kUsers = 400, kProducts = 60, kOrders = 4000;
  std::vector<Row> users, products, orders;
  for (int i = 0; i < kUsers; i++) {
    users.push_back({Value::integer(i), Value::text("u" + std::to_string(rng() % 1000)), Value::text(kCities[pick(6)]),
                     Value::integer(16 + pick(60))});
  }
  for (int i = 0; i < kProducts; i++) {
    products.push_back({Value::integer(i), Value::text(kCategory[pick(5)]), Value::real(pick(20000) / 100.0)});
  }
  for (int i = 0; i < kOrders; i++) {
    // Skewed: a few users place many of the orders.
    int user = rng() % 4 == 0 ? pick(20) : pick(kUsers + 30);  // some orders point at no user
    orders.push_back({Value::integer(i), Value::integer(user), Value::integer(pick(kProducts)),
                      Value::real(pick(100000) / 100.0), Value::text(kStatus[pick(4)]), Value::integer(1 + pick(9))});
  }
  pk.insertRows("users", users);
  pk.insertRows("products", products);
  pk.insertRows("orders", orders);
  pk.run("analyze");

  exec(sq, "begin");
  auto load = [&](const std::string& t, const std::vector<Row>& rows) {
    for (const Row& r : rows) {
      std::string sql = "insert into " + t + " values (";
      for (size_t i = 0; i < r.size(); i++) sql += (i ? ", " : "") + sqlLiteral(r[i]);
      exec(sq, sql + ")");
    }
  };
  load("users", users);
  load("products", products);
  load("orders", orders);
  exec(sq, "commit; analyze");

  auto ints = [](std::initializer_list<int64_t> xs) {
    std::vector<Value> v;
    for (int64_t x : xs) v.push_back(Value::integer(x));
    return v;
  };
  auto floats = [](std::initializer_list<double> xs) {
    std::vector<Value> v;
    for (double x : xs) v.push_back(Value::real(x));
    return v;
  };
  auto texts = [](const char* const* xs, int n, std::vector<const char*> extra) {
    std::vector<Value> v;
    for (int i = 0; i < n; i++) v.push_back(Value::text(xs[i]));
    for (const char* e : extra) v.push_back(Value::text(e));
    return v;
  };
  return {
      {"users",
       {{"id", Type::Int, ints({0, 5, 17, 100, 250, 399, 500})},
        {"name", Type::Text, {Value::text("u1"), Value::text("u5"), Value::text("u50"), Value::text("u999")}},
        {"city", Type::Text, texts(kCities, 6, {"Paris"})},
        {"age", Type::Int, ints({16, 18, 30, 45, 60, 75})}}},
      {"products",
       {{"id", Type::Int, ints({0, 3, 20, 59})},
        {"category", Type::Text, texts(kCategory, 5, {})},
        {"price", Type::Float, floats({0.5, 10, 99.99, 150})}}},
      {"orders",
       {{"id", Type::Int, ints({0, 10, 1000, 2500, 3999})},
        {"user_id", Type::Int, ints({0, 1, 7, 19, 200, 420})},
        {"product_id", Type::Int, ints({0, 5, 30, 59})},
        {"amount", Type::Float, floats({1, 100, 499.5, 900, 999.99})},
        {"status", Type::Text, texts(kStatus, 4, {})},
        {"qty", Type::Int, ints({1, 3, 5, 9})}}},
  };
}

// --- Query generation ------------------------------------------------------------

struct Ref {
  const Tab* tab;
  const Col* col;
  std::string q() const { return tab->name + "." + col->name; }
};

struct Pred {
  std::string pkql, sql;
};

Pred atom(const std::vector<Ref>& refs) {
  const Ref& r = refs[pick(static_cast<int>(refs.size()))];
  const Value& v = r.col->domain[lrng() % r.col->domain.size()];
  if (r.col->type == Type::Text) {
    int k = pick(4);
    if (k == 0) {
      std::string p = v.asText().substr(0, 1 + pick(2));
      return {"starts_with(" + r.q() + ", " + Value::text(p).literal() + ")",
              "substr(" + r.q() + ", 1, " + std::to_string(p.size()) + ") = " + sqlLiteral(Value::text(p))};
    }
    if (k == 1) {
      const Value& w = r.col->domain[lrng() % r.col->domain.size()];
      return {r.q() + " in [" + v.literal() + ", " + w.literal() + "]",
              r.q() + " in (" + sqlLiteral(v) + ", " + sqlLiteral(w) + ")"};
    }
    bool eq = k == 2;
    return {r.q() + (eq ? " == " : " != ") + v.literal(), r.q() + (eq ? " = " : " <> ") + sqlLiteral(v)};
  }
  const char* ops[][2] = {{"==", "="}, {"!=", "<>"}, {"<", "<"}, {"<=", "<="}, {">", ">"}, {">=", ">="}};
  int o = pick(6);
  return {r.q() + " " + ops[o][0] + " " + v.literal(), r.q() + " " + ops[o][1] + " " + sqlLiteral(v)};
}

Pred predicate(const std::vector<Ref>& refs, int depth = 0) {
  if (depth >= 2 || pick(3) == 0) return atom(refs);
  Pred a = predicate(refs, depth + 1), b = predicate(refs, depth + 1);
  bool conj = pick(3) != 0;
  if (pick(8) == 0) return {"not (" + a.pkql + ")", "not (" + a.sql + ")"};
  return {"(" + a.pkql + (conj ? " and " : " or ") + b.pkql + ")", "(" + a.sql + (conj ? " and " : " or ") + b.sql + ")"};
}

struct Query {
  std::string pkql, sql;
  bool ordered = false;
};

Query generate(const std::vector<Tab>& tabs) {
  const Tab& users = tabs[0];
  const Tab& products = tabs[1];
  const Tab& orders = tabs[2];

  Query q;
  std::vector<Ref> refs;
  auto addRefs = [&](const Tab& t) {
    for (const Col& c : t.cols) refs.push_back({&t, &c});
  };

  int shape = pick(4);
  std::string from_sql;
  if (shape == 0) {
    const Tab& t = tabs[pick(3)];
    q.pkql = "from " + t.name;
    from_sql = t.name;
    addRefs(t);
  } else if (shape == 1) {
    q.pkql = "from orders | join users on orders.user_id == users.id";
    from_sql = "orders join users on orders.user_id = users.id";
    addRefs(orders);
    addRefs(users);
  } else if (shape == 2) {
    q.pkql = "from products | join orders on orders.product_id == products.id";
    from_sql = "products join orders on orders.product_id = products.id";
    addRefs(products);
    addRefs(orders);
  } else {
    q.pkql = "from users | join orders on orders.user_id == users.id | join products on orders.product_id == products.id";
    from_sql = "users join orders on orders.user_id = users.id join products on orders.product_id = products.id";
    addRefs(users);
    addRefs(orders);
    addRefs(products);
  }

  std::string where;
  if (pick(5) != 0) {
    Pred p = predicate(refs);
    q.pkql += " | filter " + p.pkql;
    where = " where " + p.sql;
  }

  std::vector<std::string> out_pkql, out_sql;  // output column names / expressions
  std::string select_sql, group_sql;
  if (pick(2) == 0) {
    // Grouped.
    const Ref& key = refs[pick(static_cast<int>(refs.size()))];
    std::vector<Ref> nums;
    for (const Ref& r : refs) {
      if (r.col->type != Type::Text) nums.push_back(r);
    }
    const Ref& m = nums[pick(static_cast<int>(nums.size()))];
    // The key is a column, or sometimes a time bucket of an int column:
    // bucket(x, w) rounds down to a multiple of w, which SQL spells
    // x - ((x % w) + w) % w.
    std::string key_pkql = key.q(), key_sql = key.q(), key_name = key.col->name;
    if (key.col->type == Type::Int && pick(3) == 0) {
      std::string w = std::to_string(2 + pick(9));
      key_name = "b";
      key_pkql = "b = bucket(" + key.q() + ", " + w + ")";
      key_sql = "(" + key.q() + " - ((" + key.q() + " % " + w + ") + " + w + ") % " + w + ")";
    }
    q.pkql += " | group " + key_pkql + " { n = count(), s = sum(" + m.q() + "), lo = min(" + m.q() + "), hi = max(" +
              m.q() + ")";
    select_sql = key_sql + ", count(*), sum(" + m.q() + "), min(" + m.q() + "), max(" + m.q() + ")";
    out_pkql = {key_name, "n", "s", "lo", "hi"};
    out_sql = {"1", "2", "3", "4", "5"};
    if (pick(3) == 0) {  // percentiles, checked against SQLite's median / percentile_cont
      q.pkql += ", md = median(" + m.q() + "), q9 = p90(" + m.q() + ")";
      select_sql += ", median(" + m.q() + "), percentile_cont(" + m.q() + ", 0.9)";
      out_pkql.push_back("md");
      out_pkql.push_back("q9");
      out_sql.push_back("6");
      out_sql.push_back("7");
    }
    q.pkql += " }";
    group_sql = " group by " + key_sql;
  } else {
    // Projected.
    int k = 1 + pick(3);
    std::string sel;
    for (int i = 0; i < k; i++) {
      const Ref& r = refs[pick(static_cast<int>(refs.size()))];
      std::string name = "c" + std::to_string(i);
      sel += (i ? ", " : "") + name + " = " + r.q();
      select_sql += (i ? ", " : "") + r.q();
      out_pkql.push_back(name);
      out_sql.push_back(std::to_string(i + 1));
    }
    q.pkql += " | select " + sel;
  }

  q.sql = "select " + select_sql + " from " + from_sql + where + group_sql;

  if (pick(3) == 0) {
    // Sort by every output column, so the order (and take) is deterministic.
    std::string sp, ss;
    for (size_t i = 0; i < out_pkql.size(); i++) {
      bool desc = pick(2) == 0;
      sp += (i ? ", " : "") + std::string(desc ? "-" : "") + out_pkql[i];
      ss += (i ? ", " : "") + out_sql[i] + (desc ? " desc" : "");
    }
    int n = 1 + pick(15);
    q.pkql += " | sort " + sp + " | take " + std::to_string(n);
    q.sql += " order by " + ss + " limit " + std::to_string(n);
    q.ordered = true;
  }
  return q;
}

}  // namespace

int main(int argc, char** argv) {
  int queries = argc > 1 ? std::atoi(argv[1]) : 400;
  unsigned seed = argc > 2 ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10)) : 12345;
  rng.seed(seed);
  std::mt19937 master(seed);
  unsigned shape_seed = 0;

  pkv::makeDir("test_data");
  std::string dir = "test_data/differential";
  pkv::makeDir(dir);
  pkv::DB::destroy(dir);
  pkv::Options o;
  o.sync_writes = false;
  Engine pk(dir, o);
  sqlite3* sq = nullptr;
  sqlite3_open(":memory:", &sq);

  std::vector<Tab> tabs = buildData(pk, sq);

  struct Config {
    const char* name;
    PlannerOptions opts;
  };
  Config configs[4];
  configs[0] = {"default", PlannerOptions()};
  configs[1] = {"no-indexes", PlannerOptions()};
  configs[1].opts.use_indexes = false;
  configs[2] = {"no-join-reorder", PlannerOptions()};
  configs[2].opts.reorder_joins = false;
  configs[2].opts.use_interesting_orders = false;
  configs[3] = {"row-at-a-time", PlannerOptions()};
  configs[3].opts.vectorize = false;

  int failures = 0, empty = 0;
  for (int i = 0; i < queries; i++) {
    // Every shape runs twice with different constants: the second run reuses
    // the first run's cached plan with its own parameter values.
    if (i % 2 == 0) shape_seed = master();
    rng.seed(shape_seed);
    lrng.seed(shape_seed + 1000003u * static_cast<unsigned>(i % 2));
    Query q = generate(tabs);
    NormRows expected = runSqlite(sq, q.sql);
    if (!q.ordered) std::sort(expected.begin(), expected.end());
    if (expected.empty()) empty++;
    for (const Config& c : configs) {
      pk.plannerOptions() = c.opts;
      NormRows got;
      try {
        std::vector<Result> r = pk.run(q.pkql);
        got = normalize(r.back().rows);
      } catch (const std::exception& e) {
        std::cout << "[FAIL] query " << i << " (" << c.name << ") threw: " << e.what() << "\n  pkql: " << q.pkql << "\n";
        failures++;
        continue;
      }
      if (!q.ordered) std::sort(got.begin(), got.end());
      if (got != expected) {
        failures++;
        std::cout << "[FAIL] query " << i << " (" << c.name << "): " << got.size() << " rows, SQLite "
                  << expected.size() << "\n  pkql: " << q.pkql << "\n  sql:  " << q.sql << "\n";
        if (failures > 10) break;
      }
    }
    if (failures > 10) break;
  }
  sqlite3_close(sq);
  std::cout << "[" << (failures == 0 ? "PASS" : "FAIL") << "] " << queries << " random queries x 4 planner configs vs SQLite, "
            << failures << " mismatches (" << empty << " queries had empty results; plan cache: " << pk.planCacheHits()
            << " hits, " << pk.planCacheMisses() << " misses)\n";
  return failures == 0 ? 0 : 1;
}
