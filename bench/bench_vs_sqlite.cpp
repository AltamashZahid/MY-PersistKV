// Benchmark: PKQL on PersistKV vs SQLite, same data, same indexes, same
// cache size, the same query written in each language. Runs alternate between
// the two engines and the median of 9 is reported.
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

#include "harness.h"
#include "persistkv/file.h"
#include "pkql/engine.h"

using namespace pkql;
using namespace bench;

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
    auto t = interleavedMedians(
        9,
        [&] {
          pk_rows = 0;
          for (int i = 0; i < q.repeat; i++) pk_rows += pk.run(q.pkql(i)).back().rows.size();
        },
        [&] {
          sq_rows = 0;
          for (int i = 0; i < q.repeat; i++) sq_rows += sqlRows(sq, q.sql(i));
        });
    double pk_ms = t.first, sq_ms = t.second;
    std::printf("| %s | %.1f | %.1f | %.2fx | %s |%s\n", q.name, pk_ms, sq_ms, pk_ms / sq_ms,
                planSummary(pk, q.pkql(0)).c_str(), pk_rows == sq_rows ? "" : "  <-- ROW COUNT MISMATCH");
    std::fflush(stdout);
  }
  sqlite3_close(sq);
  return 0;
}
