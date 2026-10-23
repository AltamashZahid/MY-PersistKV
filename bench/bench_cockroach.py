"""Runs bench_events' event queries on CockroachDB, on the same data.

1. PKQL_BENCH_CSV=events.csv ./bench_events 1 <dir>   (writes the events)
2. cockroach start-single-node --insecure ...
3. python bench_cockroach.py events.csv [postgresql://root@localhost:26257/defaultdb]

The table mirrors PKQL's: ts is the primary key (rows clustered by time),
with an index on user_id. Every query is sent as plain SQL text (parsed each
time, like the other engines), timed from the client over localhost, median
of 9. Time cutoffs are taken relative to the "now" the events end at, so the
same rows qualify as in bench_events' run.
"""
import statistics
import sys
import time

import psycopg

csv_path = sys.argv[1]
dsn = sys.argv[2] if len(sys.argv) > 2 else "postgresql://root@localhost:26257/defaultdb"
HOUR, DAY = 3600000, 86400000

with open(csv_path) as f:
    now = int(f.readline())
    lines = f.readlines()

conn = psycopg.connect(dsn, autocommit=True)
cur = conn.cursor()
cur.execute("drop table if exists events")
cur.execute("create table events (ts int8 primary key, user_id int8, kind string, country string, "
            "amount float8, latency int8, index events_user (user_id))")
t0 = time.perf_counter()
# One COPY (one transaction) per 10K rows: a single 1M-row transaction runs
# past CockroachDB's commit deadline.
for start in range(0, len(lines), 10000):
    with cur.copy("copy events from stdin with csv") as copy:
        copy.write("".join(lines[start:start + 10000]))
cur.execute("analyze events")
load = time.perf_counter() - t0
print(f"CockroachDB {conn.info.parameter_status('crdb_version') or ''}".strip())
print(f"load {len(lines)} events + analyze: {load:.2f} s ({len(lines) / load:.0f} events/s)")


def ago(ms):
    return str(now - ms)


queries = [
    ("last hour: events by kind", 1, lambda i: f"select kind, count(*) from events where ts >= {ago(HOUR)} group by kind"),
    ("last 10 minutes: events per minute", 1,
     lambda i: f"select ts - ts % 60000 as m, count(*) from events where ts >= {ago(600000)} group by m order by m"),
    ("last 24h: hourly revenue from purchases", 1,
     lambda i: f"select ts - ts % 3600000 as hour, sum(amount), count(*) from events where ts >= {ago(DAY)} "
               f"and kind = 'purchase' group by hour order by hour"),
    ("last 6h: p95 latency by country", 1,
     lambda i: f"select country, percentile_cont(0.95) within group (order by latency::float8), count(*) from events "
               f"where ts >= {ago(6 * HOUR)} group by country"),
    ("last 24h: top 10 users by events", 1,
     lambda i: f"select user_id, count(*) as n from events where ts >= {ago(DAY)} group by user_id order by n desc limit 10"),
    ("one user's latest 20 events (x200)", 200,
     lambda i: f"select ts, kind from events where user_id = {1000 + (i * 37) % (50001 - 1000)} order by ts desc limit 20"),
    ("7 days: daily active users", 1,
     lambda i: "select ts - ts % 86400000 as day, count(distinct user_id) from events group by day order by day"),
    ("7 days: events and revenue by kind", 1, lambda i: "select kind, count(*), sum(amount) from events group by kind"),
    ("7 days: p50 / p99 latency by kind", 1,
     lambda i: "select kind, percentile_cont(0.5) within group (order by latency::float8), "
               "percentile_cont(0.99) within group (order by latency::float8) from events group by kind"),
]

print("| Query | CockroachDB (ms) | rows |\n|---|---|---|")
for name, repeat, sql in queries:
    def run():
        rows = 0
        for i in range(repeat):
            cur.execute(sql(i))
            rows += len(cur.fetchall())
        return rows
    rows = run()  # warm-up
    times = []
    for _ in range(9):
        t = time.perf_counter()
        run()
        times.append((time.perf_counter() - t) * 1000)
    print(f"| {name} | {statistics.median(times):.2f} | {rows} |", flush=True)
