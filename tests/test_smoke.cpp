#include "sketchydb.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

struct SingleValue {
    std::string value;
};

struct RowCount {
    int count = 0;
};

[[maybe_unused]] bool has_file_with_extension(const std::filesystem::path& directory, const char* extension) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == extension) {
            return true;
        }
    }
    return false;
}

int capture_single_value(void* user_data, int column_count, char** column_values, char** column_names) {
    (void)column_names;

    auto* captured = static_cast<SingleValue*>(user_data);
    assert(column_count == 1);
    captured->value = column_values[0] == nullptr ? "" : column_values[0];
    return 0;
}

[[maybe_unused]] int count_rows(void* user_data, int column_count, char** column_values, char** column_names) {
    (void)column_values;
    (void)column_names;

    auto* captured = static_cast<RowCount*>(user_data);
    assert(column_count == 3);
    ++captured->count;
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

    db = nullptr;
    assert(skdb_open(filename, &db) == SKDB_OK);
    assert(skdb_exec(
               db,
               "create or replace table metrics(latency_ms double, user_id varchar);",
               nullptr,
               nullptr,
               &error_message) == SKDB_OK);
    assert(error_message == nullptr);
    assert(skdb_exec(
               db,
               "insert approx_hint((latency_ms, user_id), 0.01, 0.99) "
               "into metrics(latency_ms, user_id) values "
               "(10, 'a'), (20, 'a'), (30, 'b'), (40, 'c'), (50, 'a');",
               nullptr,
               nullptr,
               &error_message) == SKDB_OK);
    assert(error_message == nullptr);

    SingleValue first_median;
    assert(skdb_exec(
               db,
               "select APPROX_MEDIAN(latency_ms, 0.01, 0.99) from metrics",
               capture_single_value,
               &first_median,
               &error_message) == SKDB_OK);
    assert(error_message == nullptr);
    SingleValue first_freq;
    assert(skdb_exec(
               db,
               "select APPROX_FREQ(user_id, 'a', 0.01, 0.99) from metrics",
               capture_single_value,
               &first_freq,
               &error_message) == SKDB_OK);
    assert(error_message == nullptr);
    assert(has_file_with_extension(sketch_dir, ".kll"));
    assert(has_file_with_extension(sketch_dir, ".freq"));
    assert(skdb_close(db) == SKDB_OK);

    db = nullptr;
    assert(skdb_open(filename, &db) == SKDB_OK);
    SingleValue reopened_median;
    assert(skdb_exec(
               db,
               "select APPROX_MEDIAN(latency_ms, 0.01, 0.99) from metrics",
               capture_single_value,
               &reopened_median,
               &error_message) == SKDB_OK);
    assert(error_message == nullptr);
    SingleValue reopened_freq;
    assert(skdb_exec(
               db,
               "select APPROX_FREQ(user_id, 'a', 0.01, 0.99) from metrics",
               capture_single_value,
               &reopened_freq,
               &error_message) == SKDB_OK);
    assert(error_message == nullptr);
    assert(reopened_median.value == first_median.value);
    assert(reopened_freq.value == first_freq.value);
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
               "insert into metrics(latency_ms) values (10), (20), (30), (40), (50);"
               "create table clicks(user_id varchar);"
               "insert into clicks(user_id) values ('hot'), ('hot'), ('hot'), ('warm'), ('warm'), ('cold');",
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
    SingleValue approximate_frequency;
    rc = skdb_exec(
        db,
        "select APPROX_FREQ(user_id, 'hot', 0.05, 0.90) from clicks",
        capture_single_value,
        &approximate_frequency,
        &error_message);
#ifdef SKDB_USE_DUCKDB
    assert(rc == SKDB_OK);
    assert(error_message == nullptr);
    assert(std::stoull(approximate_frequency.value) >= 3);

    RowCount top_k_rows;
    assert(skdb_exec(
               db,
               "select APPROX_TOP_K(user_id, 2, 0.05, 0.90) from clicks",
               count_rows,
               &top_k_rows,
               &error_message) == SKDB_OK);
    assert(error_message == nullptr);
    assert(top_k_rows.count == 2);
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
