#include "sketchydb.h"

#include <cassert>
#include <chrono>
#include <iostream>
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

void exec_or_die(skdb* db, const char* sql) {
    char* error_message = nullptr;
    const int rc = skdb_exec(db, sql, nullptr, nullptr, &error_message);
    if (rc != SKDB_OK) {
        std::cerr << "error: " << (error_message == nullptr ? skdb_errmsg(db) : error_message) << '\n';
        skdb_free(error_message);
        std::abort();
    }
}

void exec_or_die(skdb* db, const std::string& sql) {
    exec_or_die(db, sql.c_str());
}

template <typename Function>
double time_ms(Function function) {
    const auto start = std::chrono::steady_clock::now();
    function();
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

SingleValue query_single_value(skdb* db, const char* sql) {
    SingleValue value;
    char* error_message = nullptr;
    const int rc = skdb_exec(db, sql, capture_single_value, &value, &error_message);
    if (rc != SKDB_OK) {
        std::cerr << "error: " << (error_message == nullptr ? skdb_errmsg(db) : error_message) << '\n';
        skdb_free(error_message);
        std::abort();
    }
    return value;
}

}  // namespace

int main() {
#ifndef SKDB_USE_DUCKDB
    std::cout << "Skipping perf benchmark; rebuild with SKDB_USE_DUCKDB=1.\n";
    return 0;
#else
    skdb* db = nullptr;
    assert(skdb_open(":memory:", &db) == SKDB_OK);

    constexpr int kRows = 1000000;
    constexpr int kDistinct = 100000;
    constexpr int kBatchSize = 1000;

    exec_or_die(db, "create table bench_events(user_id varchar);");

    const double ingest_ms = time_ms([&] {
        for (int batch_start = 0; batch_start < kRows; batch_start += kBatchSize) {
            std::string sql = "insert into bench_events(user_id) values ";
            for (int offset = 0; offset < kBatchSize; ++offset) {
                if (offset > 0) {
                    sql += ", ";
                }
                const int value = (batch_start + offset) % kDistinct;
                sql += "('user-" + std::to_string(value) + "')";
            }
            exec_or_die(db, sql);
        }
    });

    SingleValue exact;
    const double exact_ms = time_ms([&] {
        exact = query_single_value(
            db,
            "select count(distinct user_id) as exact_count from bench_events");
    });

    SingleValue first_approx;
    const double first_approx_ms = time_ms([&] {
        first_approx = query_single_value(
            db,
            "select approx_count_distinct(user_id, 0.05, 0.90) from bench_events");
    });

    SingleValue cached_approx;
    const double cached_approx_ms = time_ms([&] {
        cached_approx = query_single_value(
            db,
            "select approx_count_distinct(user_id, 0.05, 0.90) from bench_events");
    });

    assert(!exact.value.empty());
    assert(!first_approx.value.empty());
    assert(!cached_approx.value.empty());

    std::cout << "rows=" << kRows << " distinct=" << kDistinct << '\n';
    std::cout << "sketchydb_insert_stream_time_ms=" << ingest_ms << '\n';
    std::cout << "duckdb_count_distinct=" << exact.value << " time_ms=" << exact_ms << '\n';
    std::cout << "sketchydb_hll_first_after_stream_insert=" << first_approx.value
              << " time_ms=" << first_approx_ms << '\n';
    std::cout << "sketchydb_hll_cached=" << cached_approx.value << " time_ms=" << cached_approx_ms << '\n';

    skdb_close(db);
    return 0;
#endif
}
