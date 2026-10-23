// Benchmark for the workload PKQL is built for: an append-only event log
// (clicks, views, purchases) queried by time range, then filtered and
// grouped. Same data, same index and cache size in PKQL and SQLite; runs
// alternate between the engines and the median of 9 is reported.
//
//   bench_events [scale] [dir]     scale 1 = 1M events over 7 days

#include <sqlite3.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
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
  std::string pk_dir = dir + "/pkql_events";
  pkv::makeDir(pk_dir);
  pkv::DB::destroy(pk_dir);
  std::string sq_path = dir + "/sqlite_events.db";
  std::remove(sq_path.c_str());
  std::remove((sq_path + "-wal").c_str());
  std::remove((sq_path + "-shm").c_str());

  const int kEvents = static_cast<int>(1000000 * scale);
  const int kUsers = static_cast<int>(50000 * scale) + 1;
  const int64_t kWeek = 7 * 86400000LL;

  pkv::Options opts;
  opts.sync_writes = false;  // loading speed is measured, not durability
  opts.cache_pages = 4096;   // 16 MB
  Engine pk(pk_dir, opts);
  sqlite3* sq = nullptr;
  sqlite3_open(sq_path.c_str(), &sq);
  sqlExec(sq, "pragma journal_mode = wal; pragma synchronous = off; pragma cache_size = -16384;");  // 16 MB

  // Events spread over the last 7 days, ending now, in time order (as they
  // arrive). Kinds and countries are skewed, a few users are very active,
  // and latencies have a long tail.
  std::printf("Generating %d events from %d users over 7 days...\n", kEvents, kUsers);
  std::mt19937_64 rng(42);
  const char* kinds[] = {"view", "click", "add_to_cart", "purchase", "signup"};
  const int kind_weight[] = {60, 25, 8, 5, 2};
  const char* countries[] = {"IN", "US", "BR", "DE", "GB", "JP", "FR", "CA", "AU", "MX"};
  std::vector<Row> events;
  events.reserve(kEvents);
  int64_t now = nowMillis();
  int64_t ts = now - kWeek;
  double gap = static_cast<double>(kWeek) / kEvents;
  std::exponential_distribution<double> arrival(1.0 / gap);
  std::lognormal_distribution<double> latency(4.0, 0.7);  // median ~55 ms
  for (int i = 0; i < kEvents; i++) {
    ts += 1 + static_cast<int64_t>(arrival(rng));
    int w = static_cast<int>(rng() % 100), k = 0;
    while (w >= kind_weight[k]) w -= kind_weight[k++];
    double u = std::uniform_real_distribution<double>(0, 1)(rng);
    int64_t user = static_cast<int64_t>(std::pow(u, 3.0) * kUsers);  // skewed: low ids are busy
    double amount = k == 3 ? static_cast<double>(rng() % 50000) / 100.0 : 0.0;
    events.push_back({Value::integer(ts), Value::integer(user), Value::text(kinds[k]), Value::text(countries[rng() % 10]),
                      Value::real(amount), Value::integer(static_cast<int64_t>(latency(rng)))});
  }

  pk.run(R"(
    table events (ts: int key, user_id: int, kind: text, country: text, amount: float, latency: int)
    index events_user on events (user_id)
  )");
  sqlExec(sq, R"(
    create table events (ts integer primary key, user_id integer, kind text, country text, amount real, latency integer);
    create index events_user on events (user_id);
  )");
  auto t0 = Clock::now();
  pk.insertRows("events", events);
  double pk_insert = std::chrono::duration<double>(Clock::now() - t0).count();
  pk.run("analyze");
  double pk_analyze = std::chrono::duration<double>(Clock::now() - t0).count() - pk_insert;
  pk.db().checkpoint();
  double pk_load = std::chrono::duration<double>(Clock::now() - t0).count();
  t0 = Clock::now();
  sqlLoad(sq, "events", events);
  sqlExec(sq, "analyze");
  sqlExec(sq, "pragma wal_checkpoint(truncate)");
  double sq_load = std::chrono::duration<double>(Clock::now() - t0).count();
  auto mb = [](const std::string& path) {
    std::error_code ec;
    return static_cast<double>(std::filesystem::file_size(path, ec)) / 1e6;
  };
  std::printf(
      "Load + analyze: PKQL %.2f s (%.0f events/s, %.1f MB; insert %.2f s, analyze %.2f s, checkpoint %.2f s), "
      "SQLite %.2f s (%.0f events/s, %.1f MB)\n\n",
      pk_load, kEvents / pk_load, mb(pk_dir + "/data.db"), pk_insert, pk_analyze, pk_load - pk_insert - pk_analyze, sq_load,
      kEvents / sq_load, mb(sq_path));

  // SQL has no ago(): it gets the same cutoff as a literal, computed when
  // the query is built.
  auto ago = [](int64_t ms) { return std::to_string(nowMillis() - ms); };
  auto num = [](int64_t x) { return std::to_string(x); };
  const int64_t kHour = 3600000, kDay = 86400000;
  struct Q {
    const char* name;
    int repeat;
    std::function<std::string(int)> pkql, sql;
  };
  std::vector<Q> qs = {
      {"last hour: events by kind", 1,
       [&](int) { return "from events | filter ts >= ago(1h) | group kind { n = count() }"; },
       [&](int) { return "select kind, count(*) from events where ts >= " + ago(kHour) + " group by kind"; }},
      {"last 10 minutes: events per minute", 1,
       [&](int) { return "from events | filter ts >= ago(10m) | group m = bucket(ts, 1m) { n = count() } | sort m"; },
       [&](int) {
         return "select ts - ts % 60000 as m, count(*) from events where ts >= " + ago(10 * 60000) +
                " group by m order by m";
       }},
      {"last 24h: hourly revenue from purchases", 1,
       [&](int) {
         return "from events | filter ts >= ago(1d) and kind == \"purchase\" | group hour = bucket(ts, 1h) "
                "{ revenue = sum(amount), n = count() } | sort hour";
       },
       [&](int) {
         return "select ts - ts % 3600000 as hour, sum(amount), count(*) from events where ts >= " + ago(kDay) +
                " and kind = 'purchase' group by hour order by hour";
       }},
      {"last 6h: p95 latency by country", 1,
       [&](int) { return "from events | filter ts >= ago(6h) | group country { p = p95(latency), n = count() }"; },
       [&](int) {
         return "select country, percentile_cont(latency, 0.95), count(*) from events where ts >= " + ago(6 * kHour) +
                " group by country";
       }},
      {"last 24h: top 10 users by events", 1,
       [&](int) { return "from events | filter ts >= ago(1d) | group user_id { n = count() } | sort -n | take 10"; },
       [&](int) {
         return "select user_id, count(*) as n from events where ts >= " + ago(kDay) +
                " group by user_id order by n desc limit 10";
       }},
      {"one user's latest 20 events (x200)", 200,
       [&](int i) {
         return "from events | filter user_id == " + num(1000 + (i * 37) % (kUsers - 1000)) +
                " | sort -ts | take 20 | select ts, kind";
       },
       [&](int i) {
         return "select ts, kind from events where user_id = " + num(1000 + (i * 37) % (kUsers - 1000)) +
                " order by ts desc limit 20";
       }},
      {"7 days: daily active users", 1,
       [&](int) { return "from events | group day = bucket(ts, 1d) { dau = count_distinct(user_id) } | sort day"; },
       [&](int) {
         return "select ts - ts % 86400000 as day, count(distinct user_id) from events group by day order by day";
       }},
      {"7 days: events and revenue by kind", 1,
       [&](int) { return "from events | group kind { n = count(), revenue = sum(amount) }"; },
       [&](int) { return "select kind, count(*), sum(amount) from events group by kind"; }},
      {"7 days: p50 / p99 latency by kind", 1,
       [&](int) { return "from events | group kind { p50 = median(latency), p99 = p99(latency) }"; },
       [&](int) {
         return "select kind, median(latency), percentile_cont(latency, 0.99) from events group by kind";
       }},
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
    std::printf("| %s | %.2f | %.2f | %.2fx | %s |%s\n", q.name, t.first, t.second, t.first / t.second,
                planSummary(pk, q.pkql(0)).c_str(), pk_rows == sq_rows ? "" : "  <-- ROW COUNT MISMATCH");
    std::fflush(stdout);
  }
  sqlite3_close(sq);
  return 0;
}
