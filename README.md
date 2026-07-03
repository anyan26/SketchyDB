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

As of right now, SketchyDB only support basic operations.
This is based on the belief that most DB operations are simple and repetitive. And speed-ups on those are both fun and useful.

## Pending Features
### Approx_quantile, median, p96
KLL sketch

### Heavy Hitters 
Find the most frequent items in massive dataset without counting everything.
Count-Min Sketch + heap

### APPROX_FREQ(x)
Count-MIn sketch

### Approx_Join_Size 
bottom-k / MinHash sketch

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

Latest local result, using 5 trials of 500,000 streamed inserts with 100,000
distinct users:

```text
trials=5 rows=500000 distinct=100000
duckdb_insert_stream_mean_ms=1340.37 median_ms=1340.13
sketchydb_insert_stream_with_hll_mean_ms=1407.3 median_ms=1392.04
duckdb_count_distinct_mean_ms=4.21287 median_ms=3.47904
sketchydb_hll_first_after_stream_insert_mean_ms=0.0398084 median_ms=0.038333
sketchydb_hll_cached_mean_ms=0.0322916 median_ms=0.0315
insert_overhead_mean_ms=66.9287
hll_read_speedup_vs_duckdb_mean_x=105.829
cached_hll_read_speedup_vs_duckdb_mean_x=130.463
break_even_approx_queries_after_ingest=16.0382
exact_count=100000 approx_count=101779 relative_error=0.0177914
```

The insert path paid about 67 ms of extra sketch-maintenance cost over raw
DuckDB insertion in this run. After that, `APPROX_COUNT_DISTINCT(user_id, 0.05,
0.90)` was already cache-backed and read about 106x faster than DuckDB's exact
`COUNT(DISTINCT)`. In this workload, the streaming sketch pays for itself after
about 16 approximate reads over the ingested data.
