// Shared helpers for the benchmarks against SQLite: running SQL, loading
// rows, interleaved timing, and a one-line plan summary.
#pragma once

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "pkql/engine.h"

namespace bench {

using Clock = std::chrono::steady_clock;

inline void sqlExec(sqlite3* db, const std::string& sql) {
  char* err = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    std::string e = err ? err : "?";
    sqlite3_free(err);
    throw std::runtime_error("sqlite: " + e);
  }
}

// Runs a query (prepared fresh each time, like PKQL parses each time) and
// returns the number of rows.
inline size_t sqlRows(sqlite3* db, const std::string& sql) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(db) + " in " + sql);
  }
  size_t n = 0;
  while (sqlite3_step(st) == SQLITE_ROW) n++;
  sqlite3_finalize(st);
  return n;
}

inline void sqlLoad(sqlite3* db, const std::string& table, const std::vector<pkql::Row>& rows) {
  std::string sql = "insert into " + table + " values (";
  for (size_t i = 0; i < rows[0].size(); i++) sql += i ? ", ?" : "?";
  sql += ")";
  sqlite3_stmt* st = nullptr;
  sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
  sqlExec(db, "begin");
  for (const pkql::Row& r : rows) {
    for (size_t i = 0; i < r.size(); i++) {
      int c = static_cast<int>(i) + 1;
      switch (r[i].type()) {
        case pkql::Type::Int: sqlite3_bind_int64(st, c, r[i].asInt()); break;
        case pkql::Type::Float: sqlite3_bind_double(st, c, r[i].asFloat()); break;
        case pkql::Type::Text: sqlite3_bind_text(st, c, r[i].asText().c_str(), -1, SQLITE_TRANSIENT); break;
        default: sqlite3_bind_null(st, c); break;
      }
    }
    sqlite3_step(st);
    sqlite3_reset(st);
  }
  sqlExec(db, "commit");
  sqlite3_finalize(st);
}

inline double median(std::vector<double> t) {
  std::sort(t.begin(), t.end());
  return t[t.size() / 2];
}

// Times two workloads alternately (a, b, a, b, ...) after a warm-up, so
// both see the same machine state (caches, CPU frequency, background load),
// and returns the median of each.
inline std::pair<double, double> interleavedMedians(int reps, const std::function<void()>& a,
                                                    const std::function<void()>& b) {
  a();
  b();
  std::vector<double> ta, tb;
  for (int i = 0; i < reps; i++) {
    auto t0 = Clock::now();
    a();
    ta.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    t0 = Clock::now();
    b();
    tb.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
  }
  return {median(ta), median(tb)};
}

inline std::string planSummary(pkql::Engine& pk, const std::string& q) {
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

}  // namespace bench
