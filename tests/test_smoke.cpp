#include "sketchydb.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

struct SingleValue {
    std::string value;
};

int capture_single_value(void* user_data, int column_count, char** column_values, char** column_names) {
    (void)column_names;

    auto* captured = static_cast<SingleValue*>(user_data);
    assert(column_count == 1);
    captured->value = column_values[0] == nullptr ? "" : column_values[0];
    return 0;
}

#ifdef SKDB_USE_DUCKDB
void test_persistent_sketches() {
    skdb* db = nullptr;
    char* error_message = nullptr;
    const char* filename = "build/sketchydb_persist_test.duckdb";
    const auto sketch_dir = std::filesystem::path(std::string(filename) + ".sketchydb");

    assert(skdb_open(filename, &db) == SKDB_OK);
    assert(skdb_exec(
               db,
               "create or replace table events(user_id varchar);"
               "insert into events(user_id) values ('a'), ('b'), ('a'), ('c');",
               nullptr,
               nullptr,
               &error_message) == SKDB_OK);
    assert(error_message == nullptr);

    SingleValue first_estimate;
    assert(skdb_exec(
               db,
               "select APPROX_COUNT_DISTINCT(user_id, 0.01, 0.99) from events",
               capture_single_value,
               &first_estimate,
               &error_message) == SKDB_OK);
    assert(error_message == nullptr);
    assert(std::filesystem::exists(sketch_dir));
    assert(skdb_close(db) == SKDB_OK);

    db = nullptr;
    assert(skdb_open(filename, &db) == SKDB_OK);
    SingleValue reopened_estimate;
    assert(skdb_exec(
               db,
               "select APPROX_COUNT_DISTINCT(user_id, 0.01, 0.99) from events",
               capture_single_value,
               &reopened_estimate,
               &error_message) == SKDB_OK);
    assert(error_message == nullptr);
    assert(reopened_estimate.value == first_estimate.value);
    assert(skdb_close(db) == SKDB_OK);
}
#endif

}  // namespace

int main() {
    skdb* db = nullptr;
    assert(skdb_open(":memory:", &db) == SKDB_OK);
    assert(db != nullptr);
    assert(std::strcmp(skdb_errmsg(db), "not an error") == 0);
    assert(skdb_approx_memory_bytes(db) == 0);

    char* error_message = nullptr;
    int rc = skdb_exec(db, "select 1", nullptr, nullptr, &error_message);
#ifdef SKDB_USE_DUCKDB
    assert(rc == SKDB_OK);
    assert(error_message == nullptr);
#else
    assert(rc == SKDB_ERROR);
    assert(error_message != nullptr);
    assert(std::strcmp(
               error_message,
               "DuckDB support is not compiled in; rebuild with SKDB_USE_DUCKDB=1") == 0);
    skdb_free(error_message);
#endif

#ifdef SKDB_USE_DUCKDB
    assert(skdb_exec(
               db,
               "create table events(user_id varchar);"
               "insert into events(user_id) values ('a'), ('b'), ('a'), ('c');"
               "create table metrics(latency_ms double);"
               "insert into metrics(latency_ms) values (10), (20), (30), (40), (50);",
               nullptr,
               nullptr,
               &error_message) == SKDB_OK);
    assert(error_message == nullptr);
#endif

    error_message = nullptr;
    SingleValue approximate_count;
    rc = skdb_exec(
        db,
        "select APPROX_COUNT_DISTINCT(user_id, 0.01, 0.99) from events",
        capture_single_value,
        &approximate_count,
        &error_message);
#ifdef SKDB_USE_DUCKDB
    assert(rc == SKDB_OK);
    assert(error_message == nullptr);
    assert(!approximate_count.value.empty());

    assert(skdb_exec(
               db,
               "insert into events(user_id) values ('d');",
               nullptr,
               nullptr,
               &error_message) == SKDB_OK);
    assert(error_message == nullptr);

    SingleValue approximate_count_after_insert;
    assert(skdb_exec(
               db,
               "select APPROX_COUNT_DISTINCT(user_id, 0.01, 0.99) from events",
               capture_single_value,
               &approximate_count_after_insert,
               &error_message) == SKDB_OK);
    assert(error_message == nullptr);
    assert(!approximate_count_after_insert.value.empty());
    assert(approximate_count_after_insert.value != approximate_count.value);
    assert(skdb_approx_memory_bytes(db) > 0);
#else
    assert(rc == SKDB_ERROR);
    assert(error_message != nullptr);
    assert(std::strcmp(
               error_message,
               "DuckDB support is not compiled in; rebuild with SKDB_USE_DUCKDB=1") == 0);
    skdb_free(error_message);
#endif

    error_message = nullptr;
    SingleValue approximate_median;
    rc = skdb_exec(
        db,
        "select APPROX_MEDIAN(latency_ms, 0.05, 0.90) from metrics",
        capture_single_value,
        &approximate_median,
        &error_message);
#ifdef SKDB_USE_DUCKDB
    assert(rc == SKDB_OK);
    assert(error_message == nullptr);
    assert(!approximate_median.value.empty());
#else
    assert(rc == SKDB_ERROR);
    assert(error_message != nullptr);
    assert(std::strcmp(
               error_message,
               "DuckDB support is not compiled in; rebuild with SKDB_USE_DUCKDB=1") == 0);
    skdb_free(error_message);
#endif

    error_message = nullptr;
    rc = skdb_exec(
        db,
        "select 'approx_count_distinct(user_id, 0.01, 0.99)' as literal",
        nullptr,
        nullptr,
        &error_message);
#ifdef SKDB_USE_DUCKDB
    assert(rc == SKDB_OK);
    assert(error_message == nullptr);
#else
    assert(rc == SKDB_ERROR);
    assert(error_message != nullptr);
    skdb_free(error_message);
#endif

    assert(skdb_close(db) == SKDB_OK);

#ifdef SKDB_USE_DUCKDB
    test_persistent_sketches();
#endif
}
