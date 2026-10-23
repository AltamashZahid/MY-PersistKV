// Unit tests for PKQL: encoding, language semantics, DML, persistence, and
// the optimizer's plan choices.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#include "pkql/codec.h"
#include "pkql/engine.h"
#include "persistkv/file.h"

using namespace pkql;

// --- Minimal test framework -------------------------------------------------

struct TestCase {
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

#define TEST(name)                                \
  static void name();                             \
  static Registrar registrar_##name(#name, name); \
  static void name()

#define CHECK(cond)                                                                                  \
  do {                                                                                               \
    if (!(cond)) {                                                                                   \
      throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #cond); \
    }                                                                                                \
  } while (0)

// Runs `stmt` and requires it to throw an Error whose message contains `text`.
#define CHECK_ERROR(db, stmt, text)                                                                       \
  do {                                                                                                    \
    std::string got;                                                                                      \
    try {                                                                                                 \
      (db).run(stmt);                                                                                     \
    } catch (const pkql::Error& e) {                                                                      \
      got = e.what();                                                                                     \
    }                                                                                                     \
    if (got.find(text) == std::string::npos) {                                                            \
      throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": expected error \"" + \
                               (text) + "\", got \"" + got + "\"");                                       \
    }                                                                                                     \
  } while (0)

// --- Helpers -----------------------------------------------------------------

std::string freshDir(const std::string& name) {
  pkv::makeDir("test_data");
  std::string dir = "test_data/" + name;
  pkv::makeDir(dir);
  pkv::DB::destroy(dir);
  return dir;
}

pkv::Options fast() {
  pkv::Options o;
  o.sync_writes = false;
  return o;
}

Result one(Engine& db, const std::string& stmt) {
  std::vector<Result> r = db.run(stmt);
  CHECK(r.size() == 1);
  return r[0];
}

std::string cell(Engine& db, const std::string& query) {
  Result r = one(db, query);
  CHECK(r.has_rows && r.rows.size() == 1 && r.rows[0].size() == 1);
  return r.rows[0][0].toString();
}

std::string plan(Engine& db, const std::string& query) { return one(db, "explain " + query).message; }

bool has(const std::string& haystack, const std::string& needle) { return haystack.find(needle) != std::string::npos; }

// users: n rows; orders: 10 per user; a few cities, statuses.
void loadShop(Engine& db, int users) {
  db.run(R"(
    table users (id: int key, name: text, city: text, age: int)
    table orders (id: int key, user_id: int, amount: float, status: text, created: int)
    index users_city on users (city)
    index orders_user on orders (user_id)
    index orders_created on orders (created)
  )");
  const char* cities[] = {"Pune", "Delhi", "Mumbai", "Chennai", "Kolkata", "Jaipur", "Indore", "Bhopal"};
  std::vector<Row> u, o;
  std::mt19937 rng(7);
  for (int i = 0; i < users; i++) {
    u.push_back({Value::integer(i), Value::text("user" + std::to_string(i)), Value::text(cities[i % 8]),
                 Value::integer(18 + i % 60)});
    for (int j = 0; j < 10; j++) {
      int id = i * 10 + j;
      o.push_back({Value::integer(id), Value::integer(i), Value::real((rng() % 100000) / 100.0),
                   Value::text(j == 0 ? "open" : "paid"), Value::integer(static_cast<int64_t>(rng() % 1000000))});
    }
  }
  db.insertRows("users", u);
  db.insertRows("orders", o);
  db.run("analyze");
}

// --- Encoding ------------------------------------------------------------------

TEST(key_encoding_preserves_order) {
  std::mt19937_64 rng(1);
  auto randomValue = [&](Type t) {
    switch (t) {
      case Type::Int: {
        int64_t choices[] = {0, -1, 1, INT64_MIN, INT64_MAX, static_cast<int64_t>(rng())};
        return Value::integer(choices[rng() % 6]);
      }
      case Type::Float: {
        double choices[] = {0.0, -0.0, 1.5, -1.5, 1e300, -1e-300, static_cast<double>(static_cast<int64_t>(rng())) / 7};
        return Value::real(choices[rng() % 7]);
      }
      case Type::Text: {
        std::string s;
        size_t n = rng() % 6;
        const char alphabet[] = {'\x00', '\x01', '\xff', 'a', 'b'};
        for (size_t i = 0; i < n; i++) s += alphabet[rng() % 5];
        return Value::text(s);
      }
      default: return Value::boolean(rng() % 2);
    }
  };
  for (Type t : {Type::Int, Type::Float, Type::Text, Type::Bool}) {
    for (int i = 0; i < 3000; i++) {
      Value a = rng() % 10 == 0 ? Value::null() : randomValue(t);
      Value b = rng() % 10 == 0 ? Value::null() : randomValue(t);
      std::string ea, eb;
      encodeKey(a, ea);
      encodeKey(b, eb);
      int c = compare(a, b);
      int m = ea.compare(eb);
      CHECK((c < 0) == (m < 0) && (c == 0) == (m == 0));
      std::string_view sv = ea;
      CHECK(compare(decodeKey(sv), a) == 0 && sv.empty());
    }
  }
}

// --- Language semantics --------------------------------------------------------

TEST(nulls_are_ordinary_values) {
  Engine db(freshDir("nulls"), fast());
  db.run(R"(
    table t (id: int key, x: int?)
    insert t {id: 1, x: 5}, {id: 2}, {id: 3, x: 7}
  )");
  CHECK(cell(db, "from t | filter x == null | select id") == "2");             // SQL: x = NULL matches nothing
  CHECK(cell(db, "from t | filter x != 5 | aggregate { n = count() }") == "2");  // null != 5 is true
  CHECK(cell(db, "from t | sort x | take 1 | select id") == "2");               // nulls sort first
  CHECK(cell(db, "from t | filter id == 2 | select y = x ?? 0") == "0");
  CHECK(cell(db, "from t | aggregate { s = sum(x) }") == "12");                 // aggregates skip nulls
}

TEST(division_is_real_division) {
  Engine db(freshDir("division"), fast());
  db.run("table t (id: int key, a: int) \n insert t {id: 1, a: 7}");
  CHECK(cell(db, "from t | select x = a / 2") == "3.5");  // SQL: 3
  CHECK(cell(db, "from t | select x = a // 2") == "3");
  CHECK(cell(db, "from t | select x = a % 4") == "3");
  CHECK(cell(db, "from t | select x = a / 0") == "null");
}

TEST(pipelines_compose_with_let) {
  Engine db(freshDir("let"), fast());
  db.run(R"(
    table t (id: int key, v: int)
    insert t {id: 1, v: 10}, {id: 2, v: 20}, {id: 3, v: 30}
    let big = from t | filter v >= 20
    let bigger = from big | derive w = v * 2
  )");
  CHECK(cell(db, "from bigger | aggregate { s = sum(w) }") == "100");
  CHECK(cell(db, "from t | join big on t.id == big.id | aggregate { n = count() }") == "2");
}

TEST(steps_run_in_written_order) {
  Engine db(freshDir("order"), fast());
  db.run("table t (id: int key, v: int)");
  for (int i = 1; i <= 10; i++) db.run("insert t {id: " + std::to_string(i) + ", v: " + std::to_string(i) + "}");
  // take then sort is not the same as sort then take.
  CHECK(cell(db, "from t | sort -v | take 3 | aggregate { s = sum(v) }") == "27");
  CHECK(cell(db, "from t | take 3 | sort -v | aggregate { s = sum(v) }") == "6");
  CHECK(cell(db, "from t | sort v | skip 2 | take 2 | aggregate { s = sum(v) }") == "7");
  CHECK(cell(db, "from t | group even = v % 2 == 0 { n = count() } | filter n == 5 | aggregate { g = count() }") == "2");
}

TEST(left_join_and_distinct) {
  Engine db(freshDir("left_join"), fast());
  db.run(R"(
    table a (id: int key, k: int)
    table b (id: int key, k: int, label: text)
    insert a {id: 1, k: 1}, {id: 2, k: 2}, {id: 3, k: 9}
    insert b {id: 1, k: 1, label: "x"}, {id: 2, k: 1, label: "y"}, {id: 3, k: 2, label: "x"}
  )");
  CHECK(cell(db, "from a | left join b on a.k == b.k | aggregate { n = count() }") == "4");
  CHECK(cell(db, "from a | left join b on a.k == b.k | filter b.label == null | select a.id") == "3");
  CHECK(cell(db, "from b | select label | distinct | aggregate { n = count() }") == "2");
  CHECK(cell(db, "from a | filter id > 100 | aggregate { n = count(), s = sum(k) } | select n") == "0");
}

TEST(helpful_errors) {
  Engine db(freshDir("errors"), fast());
  db.run("table t (id: int key, name: text, v: int)");
  CHECK_ERROR(db, "from t | filter v = 1", "use `==`");
  CHECK_ERROR(db, "from t | filter name > 3", "cannot compare text with int");
  CHECK_ERROR(db, "from t | filter nme == 1", "unknown column `nme`");
  CHECK_ERROR(db, "from nope", "no table or pipeline named `nope`");
  CHECK_ERROR(db, "from t | select v + 1", "give the computed column");
  CHECK_ERROR(db, "from t | filter v", "filter needs a true/false expression");
  CHECK_ERROR(db, "insert t {id: 1, name: \"a\", v: \"x\"}", "is int, got text");
  CHECK_ERROR(db, "insert t {id: 1}", "missing value for column t.name");
  CHECK_ERROR(db, "table u (x: int)", "needs a key column");
  db.run("table u (id: int key, v: int)");
  CHECK_ERROR(db, "from t | join u on id == u.id", "ambiguous column `id`");
}

// --- Writes ----------------------------------------------------------------------

TEST(insert_update_delete_keep_indexes_consistent) {
  Engine db(freshDir("dml"), fast());
  db.run(R"(
    table p (id: int key, cat: text, price: float)
    index p_cat on p (cat)
    insert p {id: 1, cat: "a", price: 10}, {id: 2, cat: "b", price: 20}, {id: 3, cat: "a", price: 30}
  )");
  CHECK_ERROR(db, "insert p {id: 2, cat: \"z\", price: 1}", "duplicate key (2)");
  CHECK(db.checkIntegrity().empty());

  db.run("from p | filter cat == \"a\" | update price = price * 2, cat = \"c\"");
  CHECK(cell(db, "from p | filter cat == \"c\" | aggregate { s = sum(price) }") == "80.0");
  CHECK(cell(db, "from p | filter cat == \"a\" | aggregate { n = count() }") == "0");
  CHECK(db.checkIntegrity().empty());

  // Changing primary keys, including two rows trading keys.
  db.run("from p | filter id == 1 | update id = 100");
  CHECK(cell(db, "from p | filter id == 100 | select cat") == "c");
  db.run("from p | filter id == 2 or id == 3 | update id = 5 - id");
  CHECK(cell(db, "from p | filter id == 3 | select cat") == "b");
  CHECK_ERROR(db, "from p | filter id == 100 | update id = 2", "duplicate key");
  CHECK(db.checkIntegrity().empty());

  db.run("from p | filter price > 50 | delete");  // prices are now 20 (id 100), 20 (id 3), 60 (id 2)
  CHECK(cell(db, "from p | aggregate { n = count() }") == "2");
  CHECK(cell(db, "from p | filter id == 2 | aggregate { n = count() }") == "0");
  CHECK(db.checkIntegrity().empty());
}

TEST(schema_data_and_stats_survive_reopen) {
  std::string dir = freshDir("reopen");
  {
    Engine db(dir, fast());
    loadShop(db, 200);
  }
  Engine db(dir, fast());
  CHECK(cell(db, "from orders | aggregate { n = count() }") == "2000");
  CHECK(db.catalog().find("orders")->stats.analyzed);
  CHECK(has(plan(db, "from orders | filter user_id == 5"), "IndexScan orders.orders_user"));
  CHECK(db.checkIntegrity().empty());
}

TEST(statements_are_atomic) {
  Engine db(freshDir("atomic"), fast());
  db.run("table t (id: int key, v: int) \n insert t {id: 1, v: 1}");
  // The second row is a duplicate, so neither row may be inserted.
  CHECK_ERROR(db, "insert t {id: 2, v: 2}, {id: 1, v: 3}", "duplicate key");
  CHECK(cell(db, "from t | aggregate { n = count() }") == "1");
  CHECK(db.checkIntegrity().empty());
}

TEST(bulk_load_checks_keys_and_types) {
  Engine db(freshDir("bulk_load"), fast());
  db.run("table t (id: int key, v: float, s: text)");
  std::vector<Row> rows;
  for (int i = 0; i < 5000; i++) rows.push_back({Value::integer(i), Value::integer(i), Value::text("x")});  // int -> float cast
  db.insertRows("t", rows);
  CHECK(cell(db, "from t | aggregate { s = sum(v) }") == "12497500.0");
  CHECK(cell(db, "from t | filter id == 7 | select v") == "7.0");
  // A key already stored, and a key repeated within one load, are both refused.
  bool threw = false;
  try {
    db.insertRows("t", {{Value::integer(9000), Value::real(1), Value::text("a")}, {Value::integer(42), Value::real(1), Value::text("b")}});
  } catch (const Error& e) {
    threw = std::string(e.what()).find("duplicate key (42)") != std::string::npos;
  }
  CHECK(threw);
  threw = false;
  try {
    db.insertRows("t", {{Value::integer(9001), Value::real(1), Value::text("a")}, {Value::integer(9001), Value::real(2), Value::text("b")}});
  } catch (const Error& e) {
    threw = std::string(e.what()).find("duplicate key (9001)") != std::string::npos;
  }
  CHECK(threw);
  CHECK(cell(db, "from t | aggregate { n = count() }") == "5000");
  CHECK(db.checkIntegrity().empty());
}

// --- Time and event analytics ------------------------------------------------------

TEST(durations_and_time_functions) {
  Engine db(freshDir("time_fns"), fast());
  db.run("table t (id: int key, ts: int?)");
  db.run("insert t {id: 1, ts: -1}, {id: 2, ts: 1700000000123}, {id: 3, ts: null}");
  CHECK(cell(db, "from t | filter id == 1 | select d = 1h + 30m + 1.5s + 250ms + 2d + 1w") ==
        std::to_string(3600000 + 1800000 + 1500 + 250 + 2 * 86400000LL + 604800000LL));
  CHECK(cell(db, "from t | filter id == 1 | select b = bucket(ts, 1s)") == "-1000");  // rounds down, also below 0
  CHECK(cell(db, "from t | filter id == 2 | select b = bucket(ts, 1d)") == "1699920000000");
  CHECK(cell(db, "from t | filter id == 2 | select b = bucket(ts, 0)") == "null");
  CHECK(cell(db, "from t | filter id == 3 | select b = bucket(ts, 1h)") == "null");
  CHECK(cell(db, "from t | filter id == 2 | select s = time_text(ts)") == "2023-11-14 22:13:20.123");
  CHECK(cell(db, "from t | filter id == 1 | select s = time_text(ts)") == "1969-12-31 23:59:59.999");
  CHECK_ERROR(db, "from t | select x = ago(id)", "ago needs a constant duration");
  CHECK_ERROR(db, "from t | select x = bucket(ts, \"1h\")", "bucket needs an int width");
}

TEST(percentile_aggregates) {
  Engine db(freshDir("percentiles"), fast());
  db.run("table t (id: int key, g: text, v: float?)");
  db.run("insert t {id: 1, g: \"a\", v: 1}, {id: 2, g: \"a\", v: 2}, {id: 3, g: \"a\", v: 10}, {id: 4, g: \"a\", v: null},"
         " {id: 5, g: \"b\", v: null}");
  // Linear interpolation between neighbours, nulls skipped (like percentile_cont).
  CHECK(cell(db, "from t | filter g == \"a\" | aggregate { m = median(v) }") == "2.0");
  CHECK(cell(db, "from t | filter g == \"a\" | aggregate { p = p95(v) }") == "9.2");
  CHECK(cell(db, "from t | filter g == \"a\" | aggregate { p = p99(v) }") == "9.84");
  CHECK(cell(db, "from t | filter g == \"b\" | aggregate { p = p90(v) }") == "null");
  CHECK(cell(db, "from t | filter g == \"zz\" | aggregate { p = median(v) }") == "null");
  CHECK_ERROR(db, "from t | aggregate { p = median(g) }", "median() needs a number");
}

TEST(time_ranges_use_the_key_and_stay_fresh) {
  Engine db(freshDir("time_range"), fast());
  db.run("table ev (ts: int key, kind: text)");
  db.run("insert ev {ts: now() - 2h, kind: \"old\"}, {ts: now() - 10m, kind: \"new\"}, {ts: now() + 400ms, kind: \"soon\"}");
  // ago() becomes a primary-key range, so only recent rows are read.
  CHECK(has(plan(db, "from ev | filter ts >= ago(1h) | aggregate { n = count() }"), "TableScan ev [ts >= ago(1h)]"));
  CHECK(cell(db, "from ev | filter ts >= ago(1h) and ts <= now() | aggregate { n = count() }") == "1");
  // The same query again runs from the plan cache, which must use the
  // current time, not the time it was planned at.
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  CHECK(cell(db, "from ev | filter ts >= ago(1h) and ts <= now() | aggregate { n = count() }") == "2");
  CHECK(db.planCacheHits() > 0);
}

// --- Optimizer -------------------------------------------------------------------

TEST(access_path_selection) {
  Engine db(freshDir("access"), fast());
  loadShop(db, 2000);  // 20,000 orders
  CHECK(has(plan(db, "from orders | filter id == 1234"), "PointLookup orders"));
  CHECK(has(plan(db, "from orders | filter id >= 100 and id < 200"), "TableScan orders [id >= 100 and id < 200]"));
  CHECK(has(plan(db, "from orders | filter user_id == 42"), "IndexScan orders.orders_user [user_id == 42]"));
  // Half the table matches: reading it in full is cheaper than 10,000 index lookups.
  CHECK(has(plan(db, "from orders | filter created > 500000"), "TableScan orders (full)"));
  CHECK(has(plan(db, "from orders | filter created > 999000"), "IndexScan orders.orders_created"));
  // 1 city in 8 is 12.5% of users: one random lookup per match would lose to
  // a full scan, but a sorted index fetch (one ordered pass decoding only the
  // matches) beats it (measured: 0.73 ms vs 0.88 ms at 10K users).
  CHECK(has(plan(db, "from users | filter city == \"Pune\""), "IndexScan users.users_city"));
}

TEST(sorted_index_fetch_keeps_results_and_order_where_needed) {
  Engine db(freshDir("sorted_fetch"), fast());
  loadShop(db, 2000);
  // Many matches: fetched in one sorted pass. Same rows as without indexes.
  const char* q = "from users | filter city == \"Delhi\" | select id, age | sort id";
  Result with = one(db, q);
  db.plannerOptions().use_indexes = false;
  Result without = one(db, q);
  CHECK(with.rows == without.rows && with.rows.size() == 250);
  db.plannerOptions().use_indexes = true;
  // When a sort is replaced by the index order, that order must survive.
  Result ordered = one(db, "from orders | sort created | take 50 | select id, created");
  CHECK(ordered.rows.size() == 50);
  for (size_t i = 1; i < ordered.rows.size(); i++) CHECK(compare(ordered.rows[i - 1][1], ordered.rows[i][1]) <= 0);
}

// Estimated rows of the plan's root, parsed from explain output.
double estimatedRows(Engine& db, const std::string& query) {
  std::string p = plan(db, query);
  size_t at = p.find("(est ");
  CHECK(at != std::string::npos);
  return std::stod(p.substr(at + 5));
}

TEST(range_bounds_on_one_column_are_estimated_together) {
  Engine db(freshDir("range_estimate"), fast());
  loadShop(db, 2000);  // order ids 0..19999
  // True answer: 1000 rows. Treating the two bounds as independent would
  // estimate P(id >= 5000) * P(id < 6000) * 20000 = 0.75 * 0.3 * 20000 = 4500.
  double est = estimatedRows(db, "from orders | filter id >= 5000 and id < 6000");
  CHECK(est > 700 && est < 1400);
  est = estimatedRows(db, "from orders | filter created >= 100000 and created < 200000");  // ~10% of rows
  CHECK(est > 1400 && est < 2800);
}

TEST(index_order_replaces_sort) {
  Engine db(freshDir("order_by_index"), fast());
  loadShop(db, 2000);
  std::string p = plan(db, "from orders | sort created | take 5");
  CHECK(has(p, "IndexScan orders.orders_created (full, for order)"));
  CHECK(!has(p, "Sort") && !has(p, "TopN"));
  CHECK(has(plan(db, "from orders | sort amount | take 5"), "TopN 5"));  // no index on amount
  CHECK(!has(plan(db, "from orders | sort id"), "Sort"));               // primary key order
  // Same answer either way.
  Result a = one(db, "from orders | sort created | take 5 | select id");
  db.plannerOptions().use_interesting_orders = false;
  Result b = one(db, "from orders | sort created | take 5 | select id");
  CHECK(a.rows == b.rows);
}

TEST(filters_move_below_joins_and_groups) {
  Engine db(freshDir("pushdown"), fast());
  loadShop(db, 500);
  std::string p = plan(db, "from orders | join users on orders.user_id == users.id | filter users.city == \"Pune\"");
  // The filter runs at the users scan (as an index range or a scan filter),
  // not above the join.
  CHECK(has(p, "TableScan users (full) filter users.city == \"Pune\"") ||
        has(p, "IndexScan users.users_city [city == \"Pune\"]"));
  CHECK(!has(p, "Filter users.city"));
  // A filter on a grouping key runs before grouping.
  p = plan(db, "from orders | group user_id { total = sum(amount) } | filter user_id == 7");
  CHECK(has(p, "IndexScan orders.orders_user [user_id == 7]"));
}

TEST(join_algorithm_and_order_selection) {
  Engine db(freshDir("joins"), fast());
  loadShop(db, 2000);
  // Few outer rows, indexed inner: look the inner table up by key.
  std::string p = plan(db, "from users | filter id < 5 | join orders on orders.user_id == users.id");
  CHECK(has(p, "IndexJoin orders via orders_user"));
  CHECK(cell(db, "from orders | join users on orders.user_id == users.id | aggregate { n = count() }") == "20000");

  // Written in a bad order (big table first, selective filter last); the
  // optimizer starts from the selective side instead.
  p = plan(db, "from orders | join users on orders.user_id == users.id | filter users.id == 3");
  CHECK(has(p, "PointLookup users"));
  CHECK(cell(db, "from orders | join users on orders.user_id == users.id | filter users.id == 3 | aggregate { n = count() }") == "10");
}

TEST(all_planner_settings_agree) {
  Engine db(freshDir("agree"), fast());
  loadShop(db, 300);
  const char* queries[] = {
      "from orders | filter user_id == 17 | select id | sort id",
      "from orders | join users on orders.user_id == users.id | filter users.city == \"Delhi\" and orders.amount > 500 "
      "| group users.age { n = count() } | sort age",
      "from users | filter age > 50 | join orders on orders.user_id == users.id | sort -orders.amount, orders.id | take 7 "
      "| select orders.id",
      "from orders | sort created, id | skip 3 | take 4 | select id",
      "from users | left join orders on orders.user_id == users.id and orders.status == \"open\" | aggregate { n = count() }",
  };
  for (const char* q : queries) {
    std::vector<Row> baseline;
    for (int mask = 0; mask < 16; mask++) {
      db.plannerOptions().reorder_joins = mask & 1;
      db.plannerOptions().use_indexes = mask & 2;
      db.plannerOptions().use_interesting_orders = mask & 4;
      db.plannerOptions().vectorize = mask & 8;
      Result r = one(db, q);
      if (mask == 0) baseline = r.rows;
      else CHECK(r.rows == baseline);
    }
  }
}

TEST(plan_cache_reuses_plans_across_values_and_stays_correct) {
  Engine db(freshDir("plan_cache"), fast());
  loadShop(db, 300);
  const char* shapes[] = {
      "from orders | filter user_id == %d | aggregate { s = sum(amount), n = count() }",
      "from orders | filter id == %d | select amount",
      "from orders | join users on orders.user_id == users.id | filter users.id == %d and orders.status != \"open\" "
      "| aggregate { n = count() }",
      "from users | filter id in [%d, 7, 11] | aggregate { n = count() }",
  };
  for (const char* shape : shapes) {
    for (int v = 0; v < 40; v++) {
      char q[512];
      std::snprintf(q, sizeof q, shape, v * 13 % 400);
      db.setPlanCache(false);
      Result fresh = one(db, q);
      db.setPlanCache(true);
      // Warm the cache with a different value first, then run this one.
      char warm[512];
      std::snprintf(warm, sizeof warm, shape, (v * 13 + 5) % 400);
      one(db, warm);
      Result cached = one(db, q);
      CHECK(cached.rows == fresh.rows);
    }
  }
  uint64_t hits = db.planCacheHits();
  for (int i = 0; i < 50; i++) one(db, "from orders | filter user_id == " + std::to_string(i) + " | select id");
  CHECK(db.planCacheHits() >= hits + 49);  // one miss, then hits

  // A new index invalidates cached plans.
  db.run("table t (id: int key, k: int)");
  std::vector<Row> rows;
  for (int i = 0; i < 3000; i++) rows.push_back({Value::integer(i), Value::integer(i % 300)});
  db.insertRows("t", rows);
  db.run("analyze t");
  one(db, "from t | filter k == 5 | select id");
  db.run("index t_k on t (k)");
  CHECK(has(plan(db, "from t | filter k == 9 | select id"), "IndexScan t.t_k"));
  CHECK(one(db, "from t | filter k == 9 | select id").rows.size() == 10);

  // Range literals are part of the cache key: different plans for different bounds.
  CHECK(has(plan(db, "from orders | filter created > 999000"), "IndexScan"));
  one(db, "from orders | filter created > 999000 | select id");
  Result wide = one(db, "from orders | filter created > 10 | aggregate { n = count() }");
  db.setPlanCache(false);
  CHECK(one(db, "from orders | filter created > 10 | aggregate { n = count() }").rows == wide.rows);
}

TEST(vectorized_execution_matches_row_execution) {
  Engine db(freshDir("vectorized"), fast());
  loadShop(db, 1000);
  db.run(R"(
    table n (id: int key, x: int?, f: float?, s: text?, b: bool?)
  )");
  std::vector<Row> rows;
  std::mt19937 rng(11);
  for (int i = 0; i < 5000; i++) {
    auto maybe = [&](Value v) { return rng() % 7 == 0 ? Value::null() : v; };  // some nulls
    rows.push_back({Value::integer(i), maybe(Value::integer(static_cast<int64_t>(rng() % 200) - 100)),
                    maybe(Value::real((rng() % 10000) / 7.0)), maybe(Value::text("s" + std::to_string(rng() % 30))),
                    maybe(Value::boolean(rng() % 2))});
  }
  db.insertRows("n", rows);
  const char* queries[] = {
      "from orders | aggregate { n = count(), s = sum(amount), lo = min(amount), hi = max(amount), a = avg(amount) }",
      "from orders | filter id >= 3000 and id < 7000 and status == \"paid\" | aggregate { s = sum(amount), n = count() }",
      "from orders | group status { n = count(), s = sum(amount), c = max(created) }",
      "from orders | group user_id { n = count() } | sort user_id",
      "from orders | sort -amount | take 10 | select id, amount",
      "from orders | filter amount > 500 | sort created, id | take 25 | select id",
      "from users | sort city, -age, id | take 40",
      "from n | aggregate { c = count(), cx = count(x), sx = sum(x), ax = avg(x), mn = min(x), mx = max(x) }",
      "from n | group s { c = count(), sf = sum(f), mins = min(s) } | sort s",
      "from n | group x { c = count() } | sort x",
      "from n | filter x < 0 | aggregate { c = count() }",
      "from n | filter x != 5 and f >= 100.5 | aggregate { c = count(), s = sum(f) }",
      "from n | filter s in [\"s1\", \"s2\", \"s7\"] or b == true | aggregate { c = count() }",
      "from n | filter starts_with(s, \"s1\") | aggregate { c = count() }",
      "from n | filter x > 1000 | aggregate { c = count(), s = sum(x), m = min(s) }",
      "from n | sort -x, id | take 15 | select id, x",
      "from n | sort s, f, id | take 15",
  };
  for (const char* q : queries) {
    db.plannerOptions().vectorize = true;
    Result v = one(db, q);
    db.plannerOptions().vectorize = false;
    Result r = one(db, q);
    if (v.rows != r.rows) throw std::runtime_error(std::string("vectorized result differs for: ") + q);
  }
  db.plannerOptions().vectorize = true;
  CHECK(has(plan(db, "from orders | group status { n = count() }"), "[vectorized]"));
  CHECK(has(plan(db, "from orders | sort -amount | take 10"), "[vectorized]"));
  CHECK(!has(plan(db, "from orders | group status, user_id { n = count() }"), "[vectorized]"));  // two keys: row path
}

TEST(explain_analyze_reports_actual_rows) {
  Engine db(freshDir("explain_analyze"), fast());
  loadShop(db, 500);
  std::string out = one(db, "explain analyze from orders | filter user_id == 3").message;
  CHECK(has(out, "[actual 10 rows"));
  CHECK(has(out, "10 rows in"));
}

int main(int argc, char** argv) {
  std::string filter = argc > 1 ? argv[1] : "";
  int passed = 0, failed = 0;
  for (const TestCase& t : registry()) {
    if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) continue;
    try {
      t.fn();
      std::cout << "[PASS] " << t.name << "\n";
      passed++;
    } catch (const std::exception& e) {
      std::cout << "[FAIL] " << t.name << ": " << e.what() << "\n";
      failed++;
    }
  }
  std::cout << "\n" << passed << " passed, " << failed << " failed\n";
  return failed == 0 ? 0 : 1;
}
