# MY-PersistKV: PKQL, a pipeline query language with a cost-based optimizer

[![CI](https://github.com/AltamashZahid/MY-PersistKV/actions/workflows/ci.yml/badge.svg)](https://github.com/AltamashZahid/MY-PersistKV/actions/workflows/ci.yml)

PKQL is a relational query language and engine built on top of
[PersistKV](https://github.com/AltamashZahid/persist_KV), my crash-safe B+Tree
key-value store (pinned here as a git submodule). It has its own language, a
System R–style cost-based optimizer, and a Volcano-style executor. Its results are
checked against SQLite with randomized differential testing.

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
| Unit tests (17) | encoding order (randomized), language semantics, error messages, update/delete index maintenance (including rows swapping primary keys), atomic statements, persistence, and **which plan the optimizer chooses** for each kind of query |
| Planner-equivalence test | every query returns identical rows under all 8 combinations of optimizer features |
| **Differential testing against SQLite** | random data loaded into both engines; thousands of randomly generated queries (filters with `and`/`or`/`not`/`in`/`starts_with`, 2- and 3-table joins, group-bys, sort with take) written in both PKQL and SQL; results must match under 3 optimizer configurations. **10,000+ executions, 0 mismatches.** |

```sh
git clone --recursive https://github.com/AltamashZahid/MY-PersistKV.git
cmake -S . -B build -G Ninja && cmake --build build
cd build && ./pkql_tests && ./differential_test 1000
./pkql mydb                      # interactive shell; type `help`
```

## Benchmark vs SQLite (honest numbers)

`bench_vs_sqlite`: the same data (10K users, 1K products, 100K orders, 2 × 40K
unindexed rows), the same indexes, a 16 MB page cache for both, and each query parsed
fresh every time in both engines. Median of 5 runs; i5-13500H, NVMe SSD, Windows 11,
g++ 16 `-O2`.

| Query | PKQL (ms) | SQLite (ms) | PKQL / SQLite | PKQL plan |
|---|---|---|---|---|
| point lookup by primary key (×1000) | 10.8 | 4.4 | 2.45× | PointLookup |
| secondary index lookup (×200) | 9.8 | 2.3 | 4.26× | IndexScan |
| primary-key range, 5000 rows | 1.3 | 0.2 | 7.88× | TableScan (key range) |
| full scan + group by | 35.7 | 30.9 | **1.16×** | HashAggregate < TableScan |
| top 10 by unindexed column | 40.5 | 3.9 | 10.3× | TopN < TableScan |
| first 10 by indexed column | 0.1 | 0.0 | — | Limit < IndexScan (no sort) |
| selective join (city + age filter) | 3.4 | 0.3 | 9.7× | IndexJoin < IndexScan |
| join all orders to products + group | 83.3 | 71.5 | **1.17×** | HashJoin |
| aggregate, then join, then top 10 | 75.6 | 68.3 | **1.11×** | TopN < HashJoin < HashAggregate |
| equi-join on unindexed columns | 86.5 | 44.5 | 1.94× | HashJoin |

**SQLite is faster on every query.** On joins, aggregation and grouping, PKQL comes
within 1.1–1.2× of it. On point lookups and small selective queries it is 2–10×
slower. The plans are the right ones; the gap is the constant cost per row and per
lookup:

- **Storage:** each PersistKV lookup costs more than a SQLite B-tree lookup.
- **Values:** rows are decoded into tagged `Value` objects; SQLite reads the record bytes in place.
- **Per-query overhead:** PKQL has no prepared statements, so it parses and plans every query from text.

How this was measured and improved: profiling showed that turning bytes into `Value`
objects took about 60% of scan time. Column pruning, decoding values in place, and
evaluating expressions by reference cut the full-scan queries by 2.8–4.6×.

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
bench/              benchmark vs SQLite
third_party/persist_KV   the storage engine (submodule)
```

## Limitations

- Writers are serialized (one statement at a time per engine).
- Keys are limited to 128 bytes, because of the storage engine.
- No subqueries inside expressions, no window functions, no multi-statement transactions.
- No prepared statements or plan cache (see the benchmark).
