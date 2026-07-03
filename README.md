# SketchyDB

```
Fast answers. Probably.
```
SketchyDB is a database that has a radical philosophy:

Being exactly correct is expensive.

Most databases spend enormous amounts of time and memory trying to answer questions perfectly.

SketchyDB asks:
```
What if we are okay with being 99% correct...and much much faster?
```

Instead of computing everything exactly, SketchyDB uses probabilistic data structures and randomized algorithms to provide answers with explicit confidence guarantees.

## Pending Features
### Heavy Hitters 
Find the most frequent items in massive dataset without counting everything.

### Approximate Cardinality
Count billions of unique values using only kilobytes of memory.

### Sketch-Accelerated Queries

### Confidence-Driven Query Planning
User specify the theoretical lower-bound they are willing to accept.
We will only provide these for atomic functions, user will decide what this implicts for their query.
More about proofs for theoretical lowerbound of each function will be located in folder proofs.

## Starter Code
SketchyDB currently has a tiny SQLite-style C API, a shell, and the beginning of
a planner/executor split. The exact execution path is designed to run through
DuckDB, while the approximate path currently supports HyperLogLog-backed
`APPROX_COUNT_DISTINCT`.

```bash
make
make test
./build/sketchydb :memory:
```

To compile the exact path against DuckDB, install DuckDB's C library/header and
build with:

```bash
make clean
make SKDB_USE_DUCKDB=1
```

With the local DuckDB bundle in your home directory:

```bash
make clean
make SKDB_USE_DUCKDB=1 DUCKDB_PREFIX=/Users/anyan/libduckdb-osx-universal
make test SKDB_USE_DUCKDB=1 DUCKDB_PREFIX=/Users/anyan/libduckdb-osx-universal
make perf SKDB_USE_DUCKDB=1 DUCKDB_PREFIX=/Users/anyan/libduckdb-osx-universal
```

Current layout:

- `include/sketchydb.h`: public C API, similar in spirit to `sqlite3.h`.
- `src/sketchydb.cpp`: opaque database handle and API implementation.
- `src/planner.cpp`: the first decision point for exact vs approximate plans.
- `src/hyperloglog.cpp`: partitionable HyperLogLog implementation.
- `src/duckdb_backend.cpp`: exact execution adapter for DuckDB.
- `shell/shell.cpp`: tiny interactive shell.
- `tests/test_smoke.cpp`: smoke test for opening, executing, errors, and closing.

First SketchyDB-specific SQL shape:

```sql
SELECT APPROX_COUNT_DISTINCT(user_id, 0.01, 0.99) FROM events;
```

The second argument is `epsilon`, the tolerated error bound, and the third
argument is `confidence`, the probability target for satisfying that bound.
`APPROX_COUNT_DISTINCT` currently executes with partitioned HyperLogLog:
SketchyDB asks DuckDB for the input expression values, feeds them into sketch
partitions, merges partitions at query time, and returns one approximate count
row.

Sketches are cached per approximate query. File-backed databases persist sketch
partitions in a sidecar directory named `<database>.sketchydb`; `:memory:`
databases keep sketches in memory only. Simple append ingestion through
`INSERT INTO table [(columns)] VALUES (...)` updates matching cached
simple-column HyperLogLog sketches directly and persists the updated partitions.
Column-list inserts also prewarm default streaming sketches for
`APPROX_COUNT_DISTINCT(column, 0.05, 0.90)`, so a later approximate query with
those bounds can avoid the initial full-table build.
Mutations that cannot be safely mapped, such as `UPDATE`, `DELETE`,
`ALTER TABLE`, `DROP`, `CREATE`, `TRUNCATE`, complex `INSERT`, or `COPY`,
invalidate cached and persisted sketches so the next approximate query rebuilds
from DuckDB. HyperLogLog is append-friendly but not delete/update-friendly, so
this keeps results safe while the mutation parser stays small.

## Benchmark Snapshot
Run:

```bash
make perf SKDB_USE_DUCKDB=1 DUCKDB_PREFIX=/Users/anyan/libduckdb-osx-universal
```

Latest local result for 1,000,000 inserted rows with 100,000 distinct users:

```text
rows=1000000 distinct=100000
sketchydb_insert_stream_time_ms=2723.09
duckdb_count_distinct=100000 time_ms=9.34604
sketchydb_hll_first_after_stream_insert=101779.144321 time_ms=0.071042
sketchydb_hll_cached=101779.144321 time_ms=0.059875
```

The insert path pays the streaming sketch maintenance cost. After that,
`APPROX_COUNT_DISTINCT(user_id, 0.05, 0.90)` is already cache-backed, so the
first approximate read avoids the old full-table HLL build.
