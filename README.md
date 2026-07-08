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
KLL sketch. `APPROX_25(x, epsilon, confidence)`,
`APPROX_MEDIAN(x, epsilon, confidence)`, and
`APPROX_75(x, epsilon, confidence)` are implemented for numeric inputs.

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
- `src/kll_sketch.cpp`: KLL quantile sketch implementation.
- `src/duckdb_backend.cpp`: exact execution adapter for DuckDB.
- `shell/shell.cpp`: tiny interactive shell.
- `tests/test_smoke.cpp`: smoke test for opening, executing, errors, and closing.

First SketchyDB-specific SQL shape:

```sql
SELECT APPROX_COUNT_DISTINCT(user_id, 0.01, 0.99) FROM events;
```

Quantile sketches:

```sql
SELECT APPROX_25(latency_ms, 0.01, 0.99) FROM events;
SELECT APPROX_MEDIAN(latency_ms, 0.01, 0.99) FROM events;
SELECT APPROX_75(latency_ms, 0.01, 0.99) FROM events;
```

The second argument is `epsilon`, the tolerated error bound, and the third
argument is `confidence`, the probability target for satisfying that bound.
`APPROX_COUNT_DISTINCT` currently executes with partitioned HyperLogLog. The
planner converts `epsilon` and `confidence` into the required HLL precision
`p`, then uses the cheapest valid sketch it can find: any cached or persisted
sketch for the same input with `p >= required_p` and the same hash seed is
valid. If no sketch satisfies the guarantee, SketchyDB asks DuckDB for the input
expression values, feeds them into a new sketch at the required precision,
persists it when possible, and returns one approximate count row.

Sketches are cached per approximate query. File-backed databases persist sketch
partitions in a sidecar directory named `<database>.sketchydb`; `:memory:`
databases keep sketches in memory only. Simple append ingestion through
`INSERT INTO table [(columns)] VALUES (...)` updates matching cached
simple-column HyperLogLog sketches directly and persists the updated partitions.
Column-list inserts also prewarm default streaming sketches for
`APPROX_COUNT_DISTINCT(column, 0.05, 0.90)`, so a later approximate query with
those bounds, or looser bounds requiring no more precision, can avoid the
initial full-table build.
Mutations that cannot be safely mapped, such as `UPDATE`, `DELETE`,
`ALTER TABLE`, `DROP`, `CREATE`, `TRUNCATE`, complex `INSERT`, or `COPY`,
invalidate cached and persisted sketches so the next approximate query rebuilds
from DuckDB. HyperLogLog is append-friendly but not delete/update-friendly, so
this keeps results safe while the mutation parser stays small.

`APPROX_25`, `APPROX_MEDIAN`, and `APPROX_75` currently use an in-memory KLL
sketch for numeric values. The three functions share a single cached KLL sketch
for the same expression and accuracy bounds, then ask that sketch for different
quantiles. Simple numeric `INSERT INTO table [(columns)] VALUES (...)` streams
into matching or default KLL sketches. KLL persistence is not implemented yet,
so file-backed databases currently persist HLL sketches but rebuild KLL sketches
after reopen.

## Benchmark Snapshot
Run:

```bash
make perf \
  SKDB_USE_DUCKDB=1 \
  DUCKDB_PREFIX=/Users/anyan/libduckdb-osx-universal \
  SKDB_HASH_SEED=424242 \
  PERF_TRIALS=5 \
  PERF_ROWS=500000 \
  PERF_DISTINCT=100000 \
  PERF_BATCH_SIZE=1000 \
  PERF_SEED=1337
```

`SKDB_HASH_SEED` overrides the HyperLogLog hash-family seed. If omitted,
SketchyDB generates the seed at `skdb_open`; file-backed databases persist it in
`<database>.sketchydb/hash_seed` so saved sketches remain reusable across
reopens. `PERF_SEED` only controls benchmark data generation.

Latest local result, using 5 trials of 500,000 randomly generated streamed
inserts over a 100,000-value user id domain:

```text
trials=5 rows=500000 distinct_domain=100000 batch_size=1000 seed=1337
duckdb_insert_stream_mean_ms=1379.43 median_ms=1367.24
sketchydb_insert_stream_with_hll_mean_ms=1441.33 median_ms=1409.01
duckdb_count_distinct_mean_ms=5.69106 median_ms=4.00879
sketchydb_hll_first_after_stream_insert_mean_ms=0.040992 median_ms=0.041375
sketchydb_hll_cached_mean_ms=0.033408 median_ms=0.031583
insert_overhead_mean_ms=61.9027
hll_read_speedup_vs_duckdb_mean_x=138.833
cached_hll_read_speedup_vs_duckdb_mean_x=170.35
break_even_approx_queries_after_ingest=10.9561
sketchydb_approx_memory_mean_bytes=66147 median_bytes=66147 mean_mib=0.0630827
exact_count=99351 approx_count=99859.5 relative_error=0.00511795
```

The insert path paid about 62 ms of extra sketch-maintenance cost over raw
DuckDB insertion in this run. After that, `APPROX_COUNT_DISTINCT(user_id, 0.05,
0.90)` was already cache-backed and read about 139x faster than DuckDB's exact
`COUNT(DISTINCT)`. In this workload, the streaming sketch pays for itself after
about 11 approximate reads over the ingested data. The memory metric estimates
SketchyDB's additional sketch/cache memory only; it does not include DuckDB's
storage, execution memory, or allocator bookkeeping.

KLL quantile benchmark:

```bash
make perf_kll \
  SKDB_USE_DUCKDB=1 \
  DUCKDB_PREFIX=/Users/anyan/libduckdb-osx-universal \
  SKDB_HASH_SEED=424242 \
  PERF_TRIALS=5 \
  PERF_ROWS=500000 \
  PERF_DISTINCT=100000 \
  PERF_BATCH_SIZE=1000 \
  PERF_SEED=1337
```

Latest local result:

```text
trials=5 rows=500000 distinct_domain=100000 batch_size=1000 seed=1337
duckdb_insert_stream_mean_ms=1330.05 median_ms=1332.25
sketchydb_insert_stream_with_kll_mean_ms=1406.11 median_ms=1406.01
duckdb_exact_median_mean_ms=6.71169 median_ms=6.61533
sketchydb_kll_first_after_stream_insert_mean_ms=0.0165416 median_ms=0.016
sketchydb_kll_cached_mean_ms=0.005825 median_ms=0.005791
insert_overhead_mean_ms=76.0659
kll_read_speedup_vs_duckdb_mean_x=405.746
cached_kll_read_speedup_vs_duckdb_mean_x=1152.22
break_even_approx_queries_after_ingest=11.3613
sketchydb_approx_memory_mean_bytes=75054 median_bytes=75054 mean_mib=0.0715771
exact_median=49894.5 approx_25=25943 approx_median=51073 approx_75=75858 median_relative_error=0.0236198
```
