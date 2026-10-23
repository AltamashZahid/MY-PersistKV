// Benchmark for the workload PKQL is built for: an append-only event log
// (clicks, views, purchases) queried by time range, then filtered and
// grouped. Same data, same index and cache size in PKQL and SQLite; runs
// alternate between the engines and the median of 9 is reported.
//
//   bench_events [scale] [dir]     scale 1 = 1M events over 7 days
//
// Built with -DDUCKDB_DIR=<path to the libduckdb release>, it can also run
// every query on DuckDB (an embedded column store), with the same data,
// all its threads and its own approximate functions. PKQL_BENCH_ENGINES
// picks the engines of a run, any of pkql, sqlite, duckdb ("pkql,sqlite" by
// default); each engine's files are only touched when it runs.
// Run DuckDB in its own process: in one process, its worker threads and
// memory traffic slow the parallel scans of the engine timed after it
// (PKQL's 7-day daily-active-users query took 154 ms there, 24 ms alone).

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

#ifdef PKQL_BENCH_DUCKDB
#include <duckdb.h>
#endif

using namespace pkql;
using namespace bench;

int main(int argc, char** argv) {
  double scale = argc > 1 ? std::atof(argv[1]) : 1.0;
  std::string dir = argc > 2 ? argv[2] : "bench_data";
  pkv::makeDir(dir);
  // Engines not in the run leave their files from earlier runs alone.
  const std::string engines = std::getenv("PKQL_BENCH_ENGINES") ? std::getenv("PKQL_BENCH_ENGINES") : "pkql,sqlite";
  const bool use_pk = engines.find("pkql") != std::string::npos;
  const bool use_sq = engines.find("sqlite") != std::string::npos;
  const bool use_dk = engines.find("duckdb") != std::string::npos;
  std::string pk_dir = dir + "/pkql_events" + (use_pk ? "" : "_idle");
  pkv::makeDir(pk_dir);
  pkv::DB::destroy(pk_dir);
  std::string sq_path = dir + "/sqlite_events" + (use_sq ? "" : "_idle") + ".db";
  std::remove(sq_path.c_str());
  std::remove((sq_path + "-wal").c_str());
  std::remove((sq_path + "-shm").c_str());

  const int kEvents = static_cast<int>(1000000 * scale);
  const int kUsers = static_cast<int>(50000 * scale) + 1;
  const int64_t kWeek = 7 * 86400000LL;

  pkv::Options opts;
  opts.sync_writes = false;  // loading speed is measured, not durability
  // Page cache for PKQL and SQLite: 16 MB, or PKQL_BENCH_CACHE_MB (DuckDB
  // manages its own memory, up to 80% of RAM by default).
  const char* cache_env = std::getenv("PKQL_BENCH_CACHE_MB");
  const long cache_mb = cache_env ? std::atol(cache_env) : 16;
  opts.cache_pages = static_cast<size_t>(cache_mb) * 256;
  Engine pk(pk_dir, opts);
  sqlite3* sq = nullptr;
  sqlite3_open(sq_path.c_str(), &sq);
  sqlExec(sq, "pragma journal_mode = wal; pragma synchronous = off; pragma cache_size = -" + std::to_string(cache_mb * 1024) + ";");

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

  // PKQL_BENCH_CSV=<file>: also write the events as CSV (first line: the
  // "now" they end at), for loading the same data into other databases
  // (see bench_cockroach.py).
  if (const char* csv = std::getenv("PKQL_BENCH_CSV")) {
    if (std::FILE* f = std::fopen(csv, "w")) {
      std::fprintf(f, "%lld\n", static_cast<long long>(now));
      for (const Row& r : events) {
        std::fprintf(f, "%lld,%lld,%s,%s,%.2f,%lld\n", static_cast<long long>(r[0].asInt()),
                     static_cast<long long>(r[1].asInt()), r[2].asText().c_str(), r[3].asText().c_str(), r[4].asFloat(),
                     static_cast<long long>(r[5].asInt()));
      }
      std::fclose(f);
    }
  }

  pk.run(R"(
    table events (ts: int key, user_id: int, kind: text, country: text, amount: float, latency: int)
    index events_user on events (user_id)
  )");
  sqlExec(sq, R"(
    create table events (ts integer primary key, user_id integer, kind text, country text, amount real, latency integer);
    create index events_user on events (user_id);
  )");
  double pk_insert = 0, pk_analyze = 0, pk_load = 0, sq_load = 0;
  auto t0 = Clock::now();
  if (use_pk) {
    pk.insertRows("events", events);
    pk_insert = std::chrono::duration<double>(Clock::now() - t0).count();
    pk.run("analyze");
    pk_analyze = std::chrono::duration<double>(Clock::now() - t0).count() - pk_insert;
    pk.db().checkpoint();
    pk_load = std::chrono::duration<double>(Clock::now() - t0).count();
  }
  if (use_sq) {
    t0 = Clock::now();
    sqlLoad(sq, "events", events);
    sqlExec(sq, "analyze");
    sqlExec(sq, "pragma wal_checkpoint(truncate)");
    sq_load = std::chrono::duration<double>(Clock::now() - t0).count();
  }
  auto mb = [](const std::string& path) {
    std::error_code ec;
    return static_cast<double>(std::filesystem::file_size(path, ec)) / 1e6;
  };
#ifdef PKQL_BENCH_DUCKDB
  std::string dk_path = dir + "/duckdb_events.db";
  std::remove(dk_path.c_str());
  std::remove((dk_path + ".wal").c_str());
  duckdb_database dk_db;
  duckdb_connection dk;
  if (duckdb_open(dk_path.c_str(), &dk_db) != DuckDBSuccess || duckdb_connect(dk_db, &dk) != DuckDBSuccess) {
    std::fprintf(stderr, "cannot open DuckDB\n");
    return 1;
  }
  auto dkExec = [&](const std::string& sql) {
    duckdb_result r;
    if (duckdb_query(dk, sql.c_str(), &r) != DuckDBSuccess) {
      std::string e = duckdb_result_error(&r);
      duckdb_destroy_result(&r);
      throw std::runtime_error("duckdb: " + e + " in " + sql);
    }
    size_t n = static_cast<size_t>(duckdb_row_count(&r));
    duckdb_destroy_result(&r);
    return n;
  };
  if (use_dk) {
  dkExec("create table events (ts bigint, user_id bigint, kind varchar, country varchar, amount double, latency bigint)");
  t0 = Clock::now();
  {
    duckdb_appender app;
    duckdb_appender_create(dk, nullptr, "events", &app);
    for (const Row& r : events) {
      duckdb_append_int64(app, r[0].asInt());
      duckdb_append_int64(app, r[1].asInt());
      duckdb_append_varchar(app, r[2].asText().c_str());
      duckdb_append_varchar(app, r[3].asText().c_str());
      duckdb_append_double(app, r[4].asFloat());
      duckdb_append_int64(app, r[5].asInt());
      duckdb_appender_end_row(app);
    }
    duckdb_appender_destroy(&app);
  }
  dkExec("create index events_user on events (user_id)");
  dkExec("checkpoint");
  double dk_load = std::chrono::duration<double>(Clock::now() - t0).count();
  std::printf("DuckDB %s: load + index + checkpoint %.2f s (%.0f events/s, %.1f MB)\n", duckdb_library_version(), dk_load,
              kEvents / dk_load, mb(dk_path));
  }
#else
  if (use_dk) {
    std::fprintf(stderr, "built without DuckDB (-DDUCKDB_DIR)\n");
    return 1;
  }
#endif
  if (use_pk) {
    std::printf("Load + analyze: PKQL %.2f s (%.0f events/s, %.1f MB; insert %.2f s, analyze %.2f s, checkpoint %.2f s)\n",
                pk_load, kEvents / pk_load, mb(pk_dir + "/data.db"), pk_insert, pk_analyze,
                pk_load - pk_insert - pk_analyze);
  }
  if (use_sq) std::printf("Load + analyze: SQLite %.2f s (%.0f events/s, %.1f MB)\n", sq_load, kEvents / sq_load, mb(sq_path));
  std::printf("\n");

  // SQL has no ago(): it gets the same cutoff as a literal, computed when
  // the query is built.
  auto ago = [](int64_t ms) { return std::to_string(nowMillis() - ms); };
  auto num = [](int64_t x) { return std::to_string(x); };
  const int64_t kHour = 3600000, kDay = 86400000;
  struct Q {
    const char* name;
    int repeat;
    std::function<std::string(int)> pkql, sql;
    std::function<std::string(int)> duck;  // DuckDB's SQL, when it differs from SQLite's
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
       },
       [&](int) {
         return "select country, quantile_cont(latency, 0.95), count(*) from events where ts >= " + ago(6 * kHour) +
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
       },
       [&](int) { return std::string("select kind, median(latency), quantile_cont(latency, 0.99) from events group by kind"); }},
      // SQLite has no approximate aggregates: it runs the exact query.
      {"7 days: daily active users, approx_count_distinct (SQLite exact)", 1,
       [&](int) { return "from events | group day = bucket(ts, 1d) { dau = approx_count_distinct(user_id) } | sort day"; },
       [&](int) {
         return "select ts - ts % 86400000 as day, count(distinct user_id) from events group by day order by day";
       },
       [&](int) {
         return std::string(
             "select ts - ts % 86400000 as day, approx_count_distinct(user_id) from events group by day order by day");
       }},
      {"7 days: approx p50 / p99 latency by kind (SQLite exact)", 1,
       [&](int) { return "from events | group kind { p50 = approx_median(latency), p99 = approx_p99(latency) }"; },
       [&](int) {
         return "select kind, median(latency), percentile_cont(latency, 0.99) from events group by kind";
       },
       [&](int) {
         return std::string("select kind, approx_quantile(latency, 0.5), approx_quantile(latency, 0.99) from events group by kind");
       }},
  };

  // Columns for the chosen engines, interleaved among them, median of 9.
  std::vector<std::string> names;
  if (use_pk) names.push_back("PKQL");
  if (use_sq) names.push_back("SQLite");
  if (use_dk) names.push_back("DuckDB");
  std::string head = "| Query |", rule = "|---|";
  for (const std::string& n : names) {
    head += " " + n + " (ms) |";
    rule += "---|";
  }
  for (size_t e = 1; use_pk && e < names.size(); e++) {
    head += " PKQL / " + names[e] + " |";
    rule += "---|";
  }
  if (use_pk) {
    head += " PKQL plan |";
    rule += "---|";
  }
  std::printf("%s\n%s\n", head.c_str(), rule.c_str());
  for (const Q& q : qs) {
    std::vector<std::function<void()>> runs;
    std::vector<size_t> rows(names.size(), 0);
    if (use_pk) {
      size_t slot = runs.size();
      runs.push_back([&, slot] {
        rows[slot] = 0;
        for (int i = 0; i < q.repeat; i++) rows[slot] += pk.run(q.pkql(i)).back().rows.size();
      });
    }
    if (use_sq) {
      size_t slot = runs.size();
      runs.push_back([&, slot] {
        rows[slot] = 0;
        for (int i = 0; i < q.repeat; i++) rows[slot] += sqlRows(sq, q.sql(i));
      });
    }
#ifdef PKQL_BENCH_DUCKDB
    if (use_dk) {
      size_t slot = runs.size();
      runs.push_back([&, slot] {
        rows[slot] = 0;
        for (int i = 0; i < q.repeat; i++) rows[slot] += dkExec(q.duck ? q.duck(i) : q.sql(i));
      });
    }
#endif
    for (auto& r : runs) r();  // warm-up
    std::vector<std::vector<double>> times(runs.size());
    for (int rep = 0; rep < 9; rep++) {
      for (size_t e = 0; e < runs.size(); e++) {
        auto t1 = Clock::now();
        runs[e]();
        times[e].push_back(std::chrono::duration<double, std::milli>(Clock::now() - t1).count());
      }
    }
    std::string line = std::string("| ") + q.name + " |";
    char buf[64];
    std::vector<double> med;
    for (auto& t : times) {
      med.push_back(median(t));
      std::snprintf(buf, sizeof buf, " %.2f |", med.back());
      line += buf;
    }
    for (size_t e = 1; use_pk && e < med.size(); e++) {
      std::snprintf(buf, sizeof buf, " %.2fx |", med[0] / med[e]);
      line += buf;
    }
    if (use_pk) line += " " + planSummary(pk, q.pkql(0)) + " |";
    bool same = true;
    for (size_t e = 1; e < rows.size(); e++) same = same && rows[e] == rows[0];
    if (!same) line += "  <-- ROW COUNT MISMATCH";
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
  }
#ifdef PKQL_BENCH_DUCKDB
  duckdb_disconnect(&dk);
  duckdb_close(&dk_db);
#endif
  sqlite3_close(sq);
  return 0;
}
