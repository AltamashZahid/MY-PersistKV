// Benchmark: PKQL on PersistKV vs SQLite, same data, same indexes, same
// cache size, the same query written in each language.
//
//   bench_vs_sqlite [scale] [dir]     scale 1 = 50K users, 500K orders

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "persistkv/file.h"
#include "pkql/engine.h"

using namespace pkql;
using Clock = std::chrono::steady_clock;

namespace {

void sqlExec(sqlite3* db, const std::string& sql) {
  char* err = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    std::string e = err ? err : "?";
    sqlite3_free(err);
    throw std::runtime_error("sqlite: " + e);
  }
}

// Runs a query (prepared fresh each time, like PKQL parses each time) and
// returns the number of rows.
size_t sqlRows(sqlite3* db, const std::string& sql) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(db) + " in " + sql);
  }
  size_t n = 0;
  while (sqlite3_step(st) == SQLITE_ROW) n++;
  sqlite3_finalize(st);
  return n;
}

void sqlLoad(sqlite3* db, const std::string& table, const std::vector<Row>& rows) {
  std::string sql = "insert into " + table + " values (";
  for (size_t i = 0; i < rows[0].size(); i++) sql += i ? ", ?" : "?";
  sql += ")";
  sqlite3_stmt* st = nullptr;
  sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
  sqlExec(db, "begin");
  for (const Row& r : rows) {
    for (size_t i = 0; i < r.size(); i++) {
      int c = static_cast<int>(i) + 1;
      switch (r[i].type()) {
        case Type::Int: sqlite3_bind_int64(st, c, r[i].asInt()); break;
        case Type::Float: sqlite3_bind_double(st, c, r[i].asFloat()); break;
        case Type::Text: sqlite3_bind_text(st, c, r[i].asText().c_str(), -1, SQLITE_TRANSIENT); break;
        default: sqlite3_bind_null(st, c); break;
      }
    }
    sqlite3_step(st);
    sqlite3_reset(st);
  }
  sqlExec(db, "commit");
  sqlite3_finalize(st);
}

double medianMs(int reps, const std::function<void()>& fn) {
  fn();  // warm up caches
  std::vector<double> t;
  for (int i = 0; i < reps; i++) {
    auto t0 = Clock::now();
    fn();
    t.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
  }
  std::sort(t.begin(), t.end());
  return t[t.size() / 2];
}

std::string planSummary(Engine& pk, const std::string& q) {
  std::string plan = pk.run("explain " + q).back().message;
  // Keep only operator names, e.g. "TopN < IndexJoin < TableScan".
  std::string out;
  size_t pos = 0;
  while (pos < plan.size()) {
    size_t end = plan.find('\n', pos);
    std::string line = plan.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    size_t start = line.find_first_not_of(" │├└─");
    if (start != std::string::npos) {
      std::string word = line.substr(start, line.find(' ', start) - start);
      if (word != "Project") out += (out.empty() ? "" : " < ") + word;
    }
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  double scale = argc > 1 ? std::atof(argv[1]) : 1.0;
  std::string dir = argc > 2 ? argv[2] : "bench_data";
  pkv::makeDir(dir);
  std::string pk_dir = dir + "/pkql";
  pkv::makeDir(pk_dir);
  pkv::DB::destroy(pk_dir);
  std::string sq_path = dir + "/sqlite.db";
  std::remove(sq_path.c_str());
  std::remove((sq_path + "-wal").c_str());
  std::remove((sq_path + "-shm").c_str());

  const int kUsers = static_cast<int>(50000 * scale);
  const int kProducts = static_cast<int>(5000 * scale);
  const int kOrders = static_cast<int>(500000 * scale);
  const int kPairs = static_cast<int>(200000 * scale);

  pkv::Options opts;
  opts.sync_writes = false;  // loading speed is not what we measure
  opts.cache_pages = 4096;   // 16 MB
  Engine pk(pk_dir, opts);
  sqlite3* sq = nullptr;
  sqlite3_open(sq_path.c_str(), &sq);
  sqlExec(sq, "pragma journal_mode = wal; pragma synchronous = off; pragma cache_size = -16384;");  // 16 MB

  std::printf("Generating %d users, %d products, %d orders, 2 x %d unindexed rows...\n", kUsers, kProducts, kOrders,
              kPairs);
  std::mt19937 rng(42);
  const char* cities[] = {"Pune", "Delhi", "Mumbai", "Chennai", "Kolkata", "Jaipur", "Indore", "Bhopal", "Agra", "Goa"};
  const char* status[] = {"open", "paid", "shipped", "returned"};
  const char* category[] = {"books", "games", "garden", "tools", "toys", "music", "food", "sports"};
  std::vector<Row> users, products, orders, a, b;
  for (int i = 0; i < kUsers; i++) {
    users.push_back({Value::integer(i), Value::text("user" + std::to_string(i)), Value::text(cities[rng() % 10]),
                     Value::integer(18 + rng() % 60)});
  }
  for (int i = 0; i < kProducts; i++) {
    products.push_back({Value::integer(i), Value::text(category[rng() % 8]), Value::real((rng() % 100000) / 100.0)});
  }
  for (int i = 0; i < kOrders; i++) {
    orders.push_back({Value::integer(i), Value::integer(rng() % kUsers), Value::integer(rng() % kProducts),
                      Value::real((rng() % 100000) / 100.0), Value::text(status[rng() % 4]),
                      Value::integer(static_cast<int64_t>(rng() % 100000000))});
  }
  for (int i = 0; i < kPairs; i++) {
    a.push_back({Value::integer(i), Value::integer(rng() % (kPairs / 2))});
    b.push_back({Value::integer(i), Value::integer(rng() % (kPairs / 2))});
  }

  pk.run(R"(
    table users (id: int key, name: text, city: text, age: int)
    table products (id: int key, category: text, price: float)
    table orders (id: int key, user_id: int, product_id: int, amount: float, status: text, created: int)
    table a (id: int key, k: int)
    table b (id: int key, k: int)
    index users_city on users (city)
    index orders_user on orders (user_id)
    index orders_created on orders (created)
  )");
  sqlExec(sq, R"(
    create table users (id integer primary key, name text, city text, age integer);
    create table products (id integer primary key, category text, price real);
    create table orders (id integer primary key, user_id integer, product_id integer, amount real, status text, created integer);
    create table a (id integer primary key, k integer);
    create table b (id integer primary key, k integer);
    create index users_city on users (city);
    create index orders_user on orders (user_id);
    create index orders_created on orders (created);
  )");

  auto t0 = Clock::now();
  pk.insertRows("users", users);
  pk.insertRows("products", products);
  pk.insertRows("orders", orders);
  pk.insertRows("a", a);
  pk.insertRows("b", b);
  pk.run("analyze");
  pk.db().checkpoint();
  double pk_load = std::chrono::duration<double>(Clock::now() - t0).count();
  t0 = Clock::now();
  sqlLoad(sq, "users", users);
  sqlLoad(sq, "products", products);
  sqlLoad(sq, "orders", orders);
  sqlLoad(sq, "a", a);
  sqlLoad(sq, "b", b);
  sqlExec(sq, "analyze");
  double sq_load = std::chrono::duration<double>(Clock::now() - t0).count();
  std::printf("Load + analyze: PKQL %.1f s, SQLite %.1f s\n\n", pk_load, sq_load);

  struct Q {
    const char* name;
    int repeat;  // run the query this many times per measurement (with different parameters)
    std::function<std::string(int)> pkql, sql;
  };
  auto num = [](int64_t x) { return std::to_string(x); };
  std::vector<Q> qs = {
      {"point lookup by primary key (x1000)", 1000,
       [&](int i) { return "from orders | filter id == " + num((i * 7919) % kOrders) + " | select amount"; },
       [&](int i) { return "select amount from orders where id = " + num((i * 7919) % kOrders); }},
      {"secondary index lookup (x200)", 200,
       [&](int i) { return "from orders | filter user_id == " + num((i * 131) % kUsers) + " | select id, amount"; },
       [&](int i) { return "select id, amount from orders where user_id = " + num((i * 131) % kUsers); }},
      {"primary-key range, 5000 rows", 1,
       [&](int) {
         return "from orders | filter id >= " + num(kOrders / 2) + " and id < " + num(kOrders / 2 + 5000) +
                " | aggregate { s = sum(amount) }";
       },
       [&](int) {
         return "select sum(amount) from orders where id >= " + num(kOrders / 2) + " and id < " + num(kOrders / 2 + 5000);
       }},
      {"full scan + group by", 1,
       [&](int) { return "from orders | group status { n = count(), s = sum(amount) }"; },
       [&](int) { return "select status, count(*), sum(amount) from orders group by status"; }},
      {"top 10 by unindexed column", 1,
       [&](int) { return "from orders | sort -amount | take 10 | select id, amount"; },
       [&](int) { return "select id, amount from orders order by amount desc limit 10"; }},
      {"first 10 by indexed column", 1,
       [&](int) { return "from orders | sort created | take 10 | select id, created"; },
       [&](int) { return "select id, created from orders order by created limit 10"; }},
      {"selective join (city + age filter)", 1,
       [&](int) {
         return "from orders | join users on orders.user_id == users.id | filter users.city == \"Pune\" and users.age < 20 "
                "| aggregate { n = count(), s = sum(orders.amount) }";
       },
       [&](int) {
         return "select count(*), sum(orders.amount) from orders join users on orders.user_id = users.id "
                "where users.city = 'Pune' and users.age < 20";
       }},
      {"join all orders to products + group", 1,
       [&](int) { return "from orders | join products on orders.product_id == products.id | group products.category { s = sum(orders.amount) }"; },
       [&](int) { return "select products.category, sum(orders.amount) from orders join products on orders.product_id = products.id group by products.category"; }},
      {"aggregate, then join, then top 10", 1,
       [&](int) {
         return "from orders | group user_id { total = sum(amount) } | join users on user_id == users.id "
                "| sort -total | take 10 | select users.name, total";
       },
       [&](int) {
         return "select users.name, t.total from (select user_id, sum(amount) as total from orders group by user_id) t "
                "join users on t.user_id = users.id order by t.total desc limit 10";
       }},
      {"equi-join on unindexed columns", 1,
       [&](int) { return "from a | join b on a.k == b.k | aggregate { n = count() }"; },
       [&](int) { return "select count(*) from a join b on a.k = b.k"; }},
  };

  std::printf("| Query | PKQL (ms) | SQLite (ms) | PKQL / SQLite | PKQL plan |\n|---|---|---|---|---|\n");
  for (const Q& q : qs) {
    size_t pk_rows = 0, sq_rows = 0;
    int reps = q.repeat > 1 ? 3 : 5;
    double pk_ms = medianMs(reps, [&] {
      pk_rows = 0;
      for (int i = 0; i < q.repeat; i++) pk_rows += pk.run(q.pkql(i)).back().rows.size();
    });
    double sq_ms = medianMs(reps, [&] {
      sq_rows = 0;
      for (int i = 0; i < q.repeat; i++) sq_rows += sqlRows(sq, q.sql(i));
    });
    std::printf("| %s | %.1f | %.1f | %.2fx | %s |%s\n", q.name, pk_ms, sq_ms, pk_ms / sq_ms,
                planSummary(pk, q.pkql(0)).c_str(), pk_rows == sq_rows ? "" : "  <-- ROW COUNT MISMATCH");
    std::fflush(stdout);
  }
  sqlite3_close(sq);
  return 0;
}
