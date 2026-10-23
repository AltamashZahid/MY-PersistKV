# MY-PersistKV: PKQL, an embedded engine for event analytics

[![CI](https://github.com/AltamashZahid/MY-PersistKV/actions/workflows/ci.yml/badge.svg)](https://github.com/AltamashZahid/MY-PersistKV/actions/workflows/ci.yml)

PKQL is an embedded database for **event data**: append-only logs of clicks, views,
purchases, API calls or app telemetry, queried by time range and then filtered and
grouped. It has its own pipeline language with time built in, a System R–style
cost-based optimizer and a vectorized executor. It runs on top of
[PersistKV](https://github.com/AltamashZahid/persist_KV), my crash-safe B+Tree
key-value store (pinned here as a git submodule). Results are checked against SQLite
with randomized differential testing.

On a 1M-event log, run against each engine alone in its own process, PKQL is faster
than **DuckDB** on 9 of 11 event queries: 3–70× on recent windows, ~65× on a user's
latest events, 1.1–4× on 7-day percentiles and distinct counts. It ties on a 7-day
group-by and loses on one high-cardinality top-N (1.4–2×). It is 3–100× faster than
**SQLite** on 10 of the 11 (1.2× slower on a user's latest events) and 2.8–230× faster
than **CockroachDB**. DuckDB loads
2.7× faster, and its files are 3× smaller. See
[the benchmark](#benchmark-event-analytics-vs-duckdb-sqlite-and-cockroachdb).

```
table events (ts: int key, user_id: int, kind: text, country: text, amount: float, latency: int)
index events_user on events (user_id)

from events
| filter ts >= ago(1d) and kind == "purchase"
| group hour = bucket(ts, 1h) { revenue = sum(amount), orders = count() }
| sort hour

from events | filter ts >= ago(6h) | group country { p95 = p95(latency), n = count() }
from events | filter user_id == 42 | sort -ts | take 20        # reads the index backwards
```

## Built for event analytics

| Feature | What it does |
|---|---|
| **Time-keyed tables** | `ts: int key` (milliseconds since 1970) stores events in time order. Appends fill pages ~89% full, and a time range is one contiguous key range. |
| **Durations and `now()` / `ago(d)`** | `500ms 30s 5m 1h 7d 2w` are int literals in ms. `filter ts >= ago(1h)` becomes a primary-key range, so only the last hour is read. Cached plans refresh `now()` on every run. |
| **`bucket(ts, 1h)`** | rounds a timestamp down to a bucket for time series (also correct below 1970); `time_text(ts)` formats it as UTC text. |
| **Percentiles** | `median`, `p90`, `p95`, `p99`: exact, interpolated like SQL's `percentile_cont`. |
| **Latest-N reads** | `sort -ts \| take 20` over a key or index range reads it **backwards** and stops after 20 rows. |
| **Vectorized, parallel aggregation** | time-window group-bys, `count_distinct` and percentiles run over 1,024-row column batches on all cores (morsel-driven); simple filters drop rows while they are being decoded. |
| **Column blocks** | older rows also get an immutable compressed columnar copy (16,384 rows per block: PFOR, delta, ALP-style scaled floats, bit-packed dictionary codes) with per-block min/max for skipping. Writes stay in the row store, which stays authoritative. |
| **Dictionary-encoded text** | low-cardinality text (`kind`, `country`) is stored as small codes; filters and group-bys compare codes, not strings. |
| **Approximate aggregates** | `approx_count_distinct` (HyperLogLog, exact up to 2,048 values) and `approx_median` / `approx_p90` / `approx_p95` / `approx_p99` (DDSketch, 1% relative error), mergeable across threads. |
| **Fast, crash-safe ingest** | inserts are batched into WAL-logged `WriteBatch`es and merged into leaf pages in key order; 270–350K events/s with a secondary index, including `analyze`. |

The query language itself:

```
table users  (id: int key, name: text, city: text, age: int?)
table orders (id: int key, user_id: int, amount: float, status: text)
index orders_user on orders (user_id)

from orders
| join users on orders.user_id == users.id
| filter users.city == "Pune" and orders.status == "paid"
| group users.name { spent = sum(orders.amount), n = count() }
| sort -spent
| take 10
```

## Why a new language: what SQL gets wrong

| SQL | PKQL |
|---|---|
| Clauses run in a fixed order unrelated to how they are written (`SELECT` is written first but evaluated near the end). | Steps run top to bottom, in the order written. `take 3 \| sort` and `sort \| take 3` mean different things, as they should. |
| Three-valued logic: `x = NULL` is never true, and `NOT (x = 5)` silently drops nulls. | `null` is an ordinary value: `x == null` works, `null != 5` is true, and nulls sort first. |
| `7 / 2` is `3` with integer columns. | `/` is real division (`3.5`); `//` is integer division. Division by zero gives `null`. |
| Reusing a query means views or nested subqueries. | `let adults = from users \| filter age >= 18`, then `from adults \| ...` composes like a function. |
| `HAVING` vs `WHERE`, and `GROUP BY` columns repeated in `SELECT`. | `filter` before or after `group`, with one syntax. |
| Implicit type coercion (`'10' > 9`). | Type errors are reported at compile time: ``cannot compare text with int``. |
| Error messages name the query, not the cause. | ``unknown column `nme` (available: t.id, t.name, …)``, ``ambiguous column `id`: write orders.id or users.id`` |

## The optimizer

Each idea below comes from a paper:

| Technique | Where | Source |
|---|---|---|
| **Filter pushdown**: into join inputs, below projections, and below `group` when a filter uses only grouping keys (HAVING → WHERE) | `rewrite()` in [src/optimizer.cpp](src/optimizer.cpp) | standard rewrite rules |
| **Access path selection**: full scan vs primary-key range vs each secondary index, costed with equi-depth histograms (`analyze`); `starts_with(col, "ab")` becomes the key range `["ab", "ac")` | `planAccess()` | Selinger et al., *Access Path Selection in a Relational DBMS* (System R, 1979) |
| **Join ordering**: dynamic programming over subsets of tables (bushy plans), choosing hash join, index nested-loop join, or nested loops for each pair | `planJoinRegion()`, `joinPair()` | Selinger 1979; the join-order study of Leis et al., *How Good Are Query Optimizers, Really?* (VLDB 2015) |
| **Interesting orders**: an index that already returns rows in the requested order replaces the sort, and with `take k` the scan stops after k rows | `planSort()` | Selinger 1979 |
| **Correlated range estimates**: `a >= x and a < y` is estimated as one histogram slice, not as two independent filters | `conjunctSelectivity()` | the correlation pitfalls in Leis et al. 2015 |
| **Column pruning**: scans decode only the columns the query uses | `prune()` | projection pushdown |
| **Volcano iterators**: operators pull rows one at a time, so `take` over a scan stops reading early | [src/exec.cpp](src/exec.cpp) | Graefe, *Volcano* (1994) |

`explain` shows the chosen plan with its estimates, and `explain analyze` also runs it
and shows the actual row counts and time for each operator:

```
pkql> explain analyze from orders | join users on orders.user_id == users.id | filter users.id == 3
Project orders.id, orders.user_id, orders.amount, orders.status, users.id, users.name, users.city, users.age  (est 10 rows, cost 54.2)  [actual 10 rows, 0.05 ms]
└─ IndexJoin orders via orders_user [user_id == users.id]  (est 10 rows, cost 54.2)  [actual 10 rows, 0.04 ms]
   └─ PointLookup users [id == 3]  (est 1 rows, cost 4)  [actual 1 rows, 0.01 ms]
10 rows in 0.05 ms
```

The query was written starting from the big `orders` table. The optimizer instead
starts from the single matching user and looks up that user's orders through the
index. (The top `Project` only restores the column order the query asked for.)

## Storage layout on PersistKV

Rows and index entries are ordinary PersistKV keys, built with an **order-preserving
("memcomparable") encoding**, so byte order equals value order:

```
r <table id> <encoded primary key>                -> encoded non-key columns
x <index id> <encoded index columns> <primary key> -> (empty)
m t <table name> / m s <table id>                  -> schema / statistics
```

A primary-key or index range query is therefore a single PersistKV range scan. Every
statement (an insert, or an update or delete including its index maintenance) is one
PersistKV `WriteBatch`, so it is atomic and crash-safe. Updates find all target rows
before writing any of them, which avoids the "Halloween problem".

## Testing

| Test | What it checks |
|---|---|
| Unit tests (30) | encoding order (randomized) and float round-trips, language semantics, error messages, update/delete index maintenance (including rows swapping primary keys), atomic statements, bulk loads, time functions and percentiles, cached time ranges that must see time move, reverse scans, dictionary encoding (reopen, rollback, full dictionaries), approximate aggregates' error bounds, parallel vs row-at-a-time results, and **which plan the optimizer chooses** for each kind of query |
| Planner-equivalence test | every query returns identical rows under all 8 combinations of optimizer features |
| **Differential testing against SQLite** | random data loaded into both engines; thousands of randomly generated queries (filters with `and`/`or`/`not`/`in`/`starts_with`, 2- and 3-table joins, group-bys including `bucket()` keys, `median`/`p90` against SQLite's `median`/`percentile_cont`, sort with take) written in both PKQL and SQL; results must match under 4 optimizer configurations (including row-at-a-time). **8,000 executions per run, 0 mismatches.** |

```sh
git clone --recursive https://github.com/AltamashZahid/MY-PersistKV.git
cmake -S . -B build -G Ninja && cmake --build build
cd build && ./pkql_tests && ./differential_test 1000
./pkql mydb                      # interactive shell; type `help`
./bench_events 1 /tmp/bench      # the event benchmark below (1M events)
```

## Benchmark: event analytics vs DuckDB, SQLite and CockroachDB

`bench_events`: 1M events over 7 days (5 kinds with a skewed mix, 10 countries, 50K
users of whom a few are very active, long-tailed latencies), keyed by `ts` with an
index on `user_id`. Every engine gets the same index and parses every query fresh;
SQL gets the same time cutoffs as literals. Median of 9 runs after a warm-up.
Hardware: i5-13500H (4 performance + 8 efficiency cores), NVMe SSD, Windows 11, g++ 16
`-O2`.

**Method.** Each engine runs **alone in its own process** (`PKQL_BENCH_ENGINES=pkql`,
then `=duckdb`, on the same data). In one process, DuckDB's worker threads and memory
traffic slowed the parallel scans of the engine timed after it. PKQL's 7-day
daily-active-users query took 154 ms there and 24 ms alone. PKQL and SQLite use a 16 MB
page cache. DuckDB manages its own memory and uses all cores, as PKQL does.

**vs DuckDB 1.3.2** (two runs each; DuckDB runs its own `approx_count_distinct` /
`approx_quantile` for the approximate rows):

| Query | PKQL (ms) | DuckDB (ms) | PKQL / DuckDB |
|---|---|---|---|
| last 10 minutes: events per minute | 0.07 / 0.08 | 2.25 / 2.83 | **0.03× / 0.03×** |
| one user's latest 20 events (×200) | 9.22 / 7.68 | 527.22 / 529.61 | **0.02× / 0.01×** |
| last hour: events by kind | 0.12 / 0.11 | 1.66 / 1.84 | **0.07× / 0.06×** |
| 7 days: `approx_median` / `approx_p99` by kind | 5.60 / 6.49 | 24.84 / 24.88 | **0.23× / 0.26×** |
| last 24h: hourly revenue from purchases | 0.92 / 0.83 | 2.58 / 2.92 | **0.36× / 0.28×** |
| last 6h: p95 latency by country | 0.92 / 0.79 | 1.59 / 1.64 | **0.58× / 0.48×** |
| 7 days: daily active users | 16.52 / 17.05 | 22.42 / 23.29 | **0.74× / 0.73×** |
| 7 days: daily active users, `approx_count_distinct` | 5.74 / 6.00 | 7.43 / 7.27 | **0.77× / 0.83×** |
| 7 days: p50 / p99 latency by kind | 37.74 / 39.58 | 42.96 / 42.27 | **0.88× / 0.94×** |
| 7 days: events and revenue by kind | 2.54 / 3.16 | 2.93 / 2.95 | 0.87× / 1.07× |
| last 24h: top 10 users by events | 5.88 / 8.91 | 4.36 / 4.42 | 1.35× / 2.02× |

**vs SQLite** (one run with both engines, interleaved):

| Query | PKQL (ms) | SQLite (ms) | PKQL / SQLite |
|---|---|---|---|
| last hour: events by kind | 0.17 | 1.48 | **0.11×** |
| last 10 minutes: events per minute | 0.08 | 0.25 | **0.30×** |
| last 24h: hourly revenue from purchases | 1.55 | 12.36 | **0.13×** |
| last 6h: p95 latency by country | 1.51 | 17.39 | **0.09×** |
| last 24h: top 10 users by events | 8.87 | 42.39 | **0.21×** |
| 7 days: daily active users | 16.58 | 526.80 | **0.03×** |
| 7 days: events and revenue by kind | 4.06 | 442.44 | **0.01×** |
| 7 days: p50 / p99 latency by kind | 40.35 | 601.89 | **0.07×** |
| 7 days: approx daily active users (SQLite exact) | 9.03 | 537.23 | **0.02×** |
| 7 days: approx p50 / p99 (SQLite exact) | 9.91 | 579.47 | **0.02×** |
| one user's latest 20 events (×200) | 6.05 | 4.91 | 1.23× |

**vs CockroachDB v25.2.4** (single node, the same CSV loaded by
`bench/bench_cockroach.py`; timed from a Python client over localhost, so each query
includes a round trip; PKQL's times from a run before column blocks):

| Query | CockroachDB (ms) | PKQL / CockroachDB |
|---|---|---|
| last hour: events by kind | 8.65 | 0.08× |
| last 24h: hourly revenue from purchases | 129.58 | 0.07× |
| last 6h: p95 latency by country | 60.75 | 0.12× |
| last 24h: top 10 users by events | 160.37 | 0.35× |
| one user's latest 20 events (×200) | 2766.16 | 0.004× |
| 7 days: daily active users | 1294.28 | 0.15× |
| 7 days: events and revenue by kind | 1365.67 | 0.06× |

CockroachDB is built for something else: transactions replicated across machines.
On one node it pays for consensus and SQL over the network, so this comparison says
little about it beyond "not for this".

| Load 1M events | PKQL (+ analyze, blocks) | SQLite (+ analyze) | DuckDB (+ index) | CockroachDB |
|---|---|---|---|---|
| time | 3.25–3.40 s | 2.28 s | **1.17–1.28 s** | 101 s |
| file size | 64.9 MB | 42.2 MB | **21.5 MB** | — |

**Where PKQL stands.** The time-ordered B+Tree reads only the newest pages for recent
windows and serves "latest N for this user" from an index read backwards. Those
queries are 3–70× faster than DuckDB. For full-history scans, the column blocks give
PKQL a compressed columnar copy of everything older than the newest 16K rows. Those
scans run on all cores with per-block skipping, which puts PKQL level with or ahead of
DuckDB on 7-day aggregations. DuckDB still wins a top-N over tens of thousands of
groups, where its partitioned parallel aggregation beats PKQL's single-threaded
grouping. It also wins on loading speed and file size: PKQL keeps both the row store
and the blocks.

## General benchmark vs SQLite

`bench_vs_sqlite`: 10K users, 1K products, 100K orders and 2 × 40K unindexed rows,
the same indexes, a 16 MB page cache for both, every query parsed fresh, runs
interleaved, median of 9. Times from the first run; ratios from both. Loading takes
0.5 s for PKQL and 0.3 s for SQLite.

| Query | PKQL (ms) | SQLite (ms) | PKQL / SQLite (2 runs) | PKQL plan |
|---|---|---|---|---|
| full scan + group by | 12.0 | 43.2 | **0.28× / 0.23×** | HashAggregate < TableScan (vectorized) |
| aggregate, then join, then top 10 | 18.6 | 72.3 | **0.26× / 0.27×** | TopN < HashJoin < HashAggregate |
| join all orders to products + group | 23.5 | 61.3 | **0.38× / 0.36×** | HashJoin < TableScan × 2 |
| equi-join on unindexed columns | 36.2 | 70.6 | **0.51× / 0.59×** | HashJoin < TableScan × 2 |
| point lookup by primary key (×1000) | 4.1 | 5.1 | **0.79× / 0.71×** | PointLookup (cached plan) |
| top 10 by unindexed column | 7.4 | 7.3 | 1.02× / 1.34× | TopN < TableScan (vectorized) |
| primary-key range, 5000 rows | 0.3 | 0.3 | 1.08× / 1.06× | TableScan (key range) |
| secondary index lookup (×200) | 3.8 | 3.3 | 1.15× / 1.09× | IndexScan (batched fetch) |
| selective join (city + age filter) | 0.6 | 0.4 | 1.61× / 1.71× | IndexJoin < IndexScan (batched fetch) |
| first 10 by indexed column | < 0.1 | < 0.1 | 1.69× / 1.72× | Limit < IndexScan (sort removed) |

Ratios move with machine state (this machine was 2–3× faster during these runs than
in earlier ones), so the README keeps both runs. Scans, aggregation and joins are
2–4× faster than SQLite. Point and index lookups and small queries, where fixed
per-query and per-lookup costs dominate, are within 0.7–1.7×.

### How the gap was closed (each step measured first)

The first version was 2–10× slower than SQLite on every query. In order:

| Change | What the profile showed | Effect |
|---|---|---|
| Column pruning; evaluating expressions by reference | building `Value`s took ~60% of scan time | full-scan queries 2.8–4.6× faster |
| Plan cache with auto-parameterization | a point lookup spent 80% of its time parsing and planning | point lookups 1.44× → 1.10× of SQLite |
| Zero-copy scans (PersistKV `scanRaw`); filtering inside the scan callback; reusing row buffers | every key and value was copied twice before decoding | 5,000-row range 2.5 → 0.95 ms |
| Calibrated cost constants (lookup ≈ 10 sequential rows, measured) | the model assumed 4, so it over-used indexes | better plan choices |
| PersistKV: zero-copy point lookups; **slotted pages** with binary search | every lookup copied 3 pages and compared keys one by one (~170 compares) | lookups 1.4 → 0.8 µs |
| PersistKV: **append-aware splits** | ascending loads left pages 50% full | pages ~75% full |
| Compact encoding: varint values, variable-length order-preserving integer keys, 1-byte ids | the database was 4× SQLite's size (29 MB vs 7 MB) and spilled out of the cache | 29 MB → 16 MB |
| **Sorted index fetch** (like PostgreSQL's bitmap heap scan) | 1,000 matches meant 1,000 random lookups | one ordered pass; 0.73 vs 0.88 ms against a full scan |
| Flat hash join; decode plans that stop at the last needed column | node allocation per build row; every column parsed | unindexed equi-join 61 → 28 ms (now faster than SQLite) |
| **Vectorized execution**: scans feed 1,024-row column batches with selection vectors into aggregate and top-N kernels | per-row `Value` decoding and virtual calls | full scan + group by 0.47× → 0.26× of SQLite |
| PersistKV: array page table; zero-copy `getRaw` decoding rows straight from the cached page | a hash-map probe per page and a row copy per lookup | random lookup ~0.8 → ~0.6 µs; point lookups 0.85× → 0.6× |
| Index entries read in place; row keys built in the scan callback | each index entry was copied into two strings and decoded into `Value`s | the Pune users scan ~0.55 → ~0.38 ms |
| PersistKV: **batched lookups** (`multiGetRaw`) for sorted row keys; fast key sort (8-byte prefixes, already-sorted runs skipped) | sorting `std::string`s cost more than the lookups; each lookup descended from the root | sorted fetches ~2× faster; index-join probes batched too |
| Recalibrated sorted-fetch costs | the planner used an index range at 20% selectivity (6.1 ms) when a scan takes 3.4 ms | it now picks the scan |

Then, for event analytics (`bench_events`, 1M events):

| Change | What the profile showed | Effect |
|---|---|---|
| PersistKV: puts change the leaf page in place; put-only batches merge runs of keys into each leaf, and split it in place | every put decoded and re-encoded each page on its path into `Node`s (~100 allocations per level) | ingest 11.3 → ~1 µs/row; raw batched puts 18 → 0.4 µs |
| PersistKV: slicing-by-8 CRC32; binary-search leaf merges | byte-at-a-time CRC; linear scans to find positions | WAL appends ~2.5× faster |
| PersistKV: rarer checkpoints (32 MB / 8,192 pages) | random index inserts re-dirtied the same pages: 75 checkpoints wrote 1.4 GB for a 75 MB file | load 15.7 → 4.5 s |
| PersistKV: dirty pages kept out of the eviction list | after a load, every cache miss walked past thousands of dirty pages | `analyze` after load 3.1 → 0.75 s |
| Batched duplicate-key check; no row copies on insert | a hash set and one `get` per row; every row copied | 1.9 → 0.6–1.0 µs/row |
| `analyze` with HyperLogLog and a 30K-row sample | exact hash sets (one allocation per value) and a 200K-row reservoir | 5.5 → 0.25–0.75 s |
| Reverse scans (PersistKV `scanRawReverse`) for descending sorts | "latest 20 events of a user" fetched and sorted all of them | 11.4× → 1.1× of SQLite |
| Vectorized `bucket()` keys, `count_distinct`, percentiles; filters applied while decoding | those aggregates ran row at a time | per-minute counts 1.36× → 0.34×, daily active users 1.05× → 0.37× |
| Whole-valued floats stored as varints | 95% of rows stored `amount = 0.0` in 8 bytes | 74.6 → 67.2 MB |
| **Dictionary encoding** of text columns (Abadi et al., SIGMOD 2006); filters and group-bys on codes | `kind == "purchase"` decoded and compared strings on 95% failing rows | hourly revenue 1.25× → 1.01× of SQLite; 67.2 → 59.2 MB |
| **Morsel-driven parallelism** (Leis et al., SIGMOD 2014): morsels cut at B+Tree separators, partitioned parallel merge | 7-day scans used one core | 7-day totals by kind 0.24× → 0.08× of SQLite |
| PersistKV: **striped page cache**, per-reader file handles | one cache mutex, and Windows serializing reads per handle, capped scans at ~4 threads | 16-thread 7-day scan 57.7 → 29.5 ms (4.9 ms fully cached) |
| Kahan–Babuška–Neumaier compensated sums | parallel partial sums changed the last digits | float sums independent of order |
| **HyperLogLog** and **DDSketch** approximate aggregates (Heule et al., EDBT 2013; Masson et al., VLDB 2019) | exact distinct sets and sorted value lists per group | approx daily active users 0.20× → 0.08× of SQLite |
| **Column blocks** (SQL Server columnstore, Larson et al. SIGMOD 2011; Data Blocks, Lang et al. SIGMOD 2016) with PFOR (Zukowski et al. ICDE 2006), delta and ALP-style float encodings and per-block min/max | DuckDB's columnar scans were 7× faster on 7-day group-bys | 7-day totals by kind 76 → 2.5 ms |
| Worker pool; per-thread decode buffers; one-load bit-unpacking; columnar aggregate loops; count() from block metadata | thread creation (~1.5 ms a query on Windows) and 128 KB allocations under the heap lock capped scaling at 4 threads | 16-thread block scans scale (12.4 → 2.3 ms) |
| Flat, pre-sized grouping tables; open-addressing hash maps and sets | node-based hash tables and per-group allocations | top users of a day 24 → 6 ms |
| Each benchmark engine in its own process | DuckDB's threads slowed whichever engine ran after it | honest numbers |

Tools used: phase timing (parse / plan / execute), layer-by-layer scan timing
(storage → decode → cursor → operator), and minimum-of-N runs to filter out a noisy
machine.

## Layout

```
include/pkql, src/
  value, codec      values, order-preserving key encoding, row encoding
  storage           key layout, catalog, statistics, cursor over PersistKV
  parser            lexer + recursive-descent parser for PKQL
  expr              binding, type checking, evaluation
  planner           pipeline -> logical plan
  optimizer         rewrites, cardinality estimation, access paths, join DP, pruning, explain
  exec              Volcano operators: scans, hash/index/nested-loop joins, hash aggregate, sort, top-N
  engine            statements, DDL/DML, analyze
tools/pkql.cpp      interactive shell
tests/              unit tests, differential test vs SQLite
bench/              bench_events (event log) and bench_vs_sqlite (general), shared harness.h
third_party/persist_KV   the storage engine (submodule)
```

## Limitations

- Writers are serialized (one statement at a time per engine).
- Keys are limited to 128 bytes, because of the storage engine.
- No subqueries inside expressions, no window functions, no multi-statement transactions.
- Vectorized execution covers scans feeding aggregates and top-N; joins, sorts and filters above them still run row at a time.
- Reverse scans serve descending sorts over a single table or index; a descending sort over a join still sorts.
- Timestamps are ints (ms since 1970, UTC); there is no separate time type or time-zone handling.
- Files are 1.5× SQLite's and 3× DuckDB's size for the same events (the row store plus column blocks), and loading is 1.4× slower than SQLite and 2.7× slower than DuckDB.
- A top-N over tens of thousands of groups runs on one thread and is up to 2× slower than DuckDB.
- Column blocks cover tables with a single int key; other tables are scanned row by row (still vectorized).
- `PKQL_THREADS` (environment) caps the threads of parallel scans; by default, all cores up to 16.
