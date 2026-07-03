#include "sketchydb.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#ifdef SKDB_USE_DUCKDB
#include <duckdb.h>
#endif

namespace {

constexpr int kTrials = 5;
constexpr int kRows = 500000;
constexpr int kDistinct = 100000;
constexpr int kBatchSize = 1000;
constexpr const char* kExactSql = "select count(distinct user_id) as exact_count from bench_events";
constexpr const char* kApproxSql = "select approx_count_distinct(user_id, 0.05, 0.90) from bench_events";

struct SingleValue {
    std::string value;
};

struct TrialResult {
    double duckdb_insert_ms = 0.0;
    double sketchydb_insert_stream_ms = 0.0;
    double duckdb_count_distinct_ms = 0.0;
    double sketchydb_hll_first_ms = 0.0;
    double sketchydb_hll_cached_ms = 0.0;
    double exact_count = 0.0;
    double approximate_count = 0.0;
};

int capture_single_value(void* user_data, int column_count, char** column_values, char** column_names) {
    (void)column_names;

    auto* captured = static_cast<SingleValue*>(user_data);
    assert(column_count == 1);
    captured->value = column_values[0] == nullptr ? "" : column_values[0];
    return 0;
}

template <typename Function>
double time_ms(Function function) {
    const auto start = std::chrono::steady_clock::now();
    function();
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

std::string insert_sql_for_batch(int batch_start) {
    std::string sql = "insert into bench_events(user_id) values ";
    for (int offset = 0; offset < kBatchSize; ++offset) {
        if (offset > 0) {
            sql += ", ";
        }
        const int value = (batch_start + offset) % kDistinct;
        sql += "('user-" + std::to_string(value) + "')";
    }
    return sql;
}

void exec_or_die(skdb* db, const char* sql) {
    char* error_message = nullptr;
    const int rc = skdb_exec(db, sql, nullptr, nullptr, &error_message);
    if (rc != SKDB_OK) {
        std::cerr << "sketchydb error: " << (error_message == nullptr ? skdb_errmsg(db) : error_message) << '\n';
        skdb_free(error_message);
        std::abort();
    }
}

void exec_or_die(skdb* db, const std::string& sql) {
    exec_or_die(db, sql.c_str());
}

SingleValue query_single_value(skdb* db, const char* sql) {
    SingleValue value;
    char* error_message = nullptr;
    const int rc = skdb_exec(db, sql, capture_single_value, &value, &error_message);
    if (rc != SKDB_OK) {
        std::cerr << "sketchydb error: " << (error_message == nullptr ? skdb_errmsg(db) : error_message) << '\n';
        skdb_free(error_message);
        std::abort();
    }
    return value;
}

#ifdef SKDB_USE_DUCKDB
void duckdb_query_or_die(duckdb_connection connection, const char* sql) {
    duckdb_result result;
    if (duckdb_query(connection, sql, &result) == DuckDBError) {
        std::cerr << "duckdb error: " << duckdb_result_error(&result) << '\n';
        duckdb_destroy_result(&result);
        std::abort();
    }
    duckdb_destroy_result(&result);
}

void duckdb_query_or_die(duckdb_connection connection, const std::string& sql) {
    duckdb_query_or_die(connection, sql.c_str());
}

double run_duckdb_insert_baseline() {
    duckdb_database database = nullptr;
    duckdb_connection connection = nullptr;
    if (duckdb_open(nullptr, &database) == DuckDBError) {
        std::abort();
    }
    if (duckdb_connect(database, &connection) == DuckDBError) {
        duckdb_close(&database);
        std::abort();
    }

    duckdb_query_or_die(connection, "create table bench_events(user_id varchar)");
    const double insert_ms = time_ms([&] {
        for (int batch_start = 0; batch_start < kRows; batch_start += kBatchSize) {
            duckdb_query_or_die(connection, insert_sql_for_batch(batch_start));
        }
    });

    duckdb_disconnect(&connection);
    duckdb_close(&database);
    return insert_ms;
}
#endif

TrialResult run_trial() {
    TrialResult result;
#ifdef SKDB_USE_DUCKDB
    result.duckdb_insert_ms = run_duckdb_insert_baseline();

    skdb* db = nullptr;
    assert(skdb_open(":memory:", &db) == SKDB_OK);
    exec_or_die(db, "create table bench_events(user_id varchar)");

    result.sketchydb_insert_stream_ms = time_ms([&] {
        for (int batch_start = 0; batch_start < kRows; batch_start += kBatchSize) {
            exec_or_die(db, insert_sql_for_batch(batch_start));
        }
    });

    SingleValue exact;
    result.duckdb_count_distinct_ms = time_ms([&] {
        exact = query_single_value(db, kExactSql);
    });

    SingleValue first_approx;
    result.sketchydb_hll_first_ms = time_ms([&] {
        first_approx = query_single_value(db, kApproxSql);
    });

    SingleValue cached_approx;
    result.sketchydb_hll_cached_ms = time_ms([&] {
        cached_approx = query_single_value(db, kApproxSql);
    });

    result.exact_count = std::stod(exact.value);
    result.approximate_count = std::stod(first_approx.value);
    assert(!cached_approx.value.empty());

    skdb_close(db);
#endif
    return result;
}

double mean(const std::vector<double>& values) {
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto midpoint = values.size() / 2;
    if (values.size() % 2 == 1) {
        return values[midpoint];
    }
    return (values[midpoint - 1] + values[midpoint]) / 2.0;
}

std::vector<double> collect(const std::vector<TrialResult>& results, double TrialResult::*field) {
    std::vector<double> values;
    values.reserve(results.size());
    for (const auto& result : results) {
        values.push_back(result.*field);
    }
    return values;
}

void print_metric(const char* name, const std::vector<double>& values) {
    std::cout << name << "_mean_ms=" << mean(values)
              << " median_ms=" << median(values) << '\n';
}

}  // namespace

int main() {
#ifndef SKDB_USE_DUCKDB
    std::cout << "Skipping perf benchmark; rebuild with SKDB_USE_DUCKDB=1.\n";
    return 0;
#else
    std::vector<TrialResult> results;
    results.reserve(kTrials);
    for (int trial = 0; trial < kTrials; ++trial) {
        results.push_back(run_trial());
    }

    const auto duckdb_insert = collect(results, &TrialResult::duckdb_insert_ms);
    const auto sketchy_insert = collect(results, &TrialResult::sketchydb_insert_stream_ms);
    const auto exact_query = collect(results, &TrialResult::duckdb_count_distinct_ms);
    const auto hll_first = collect(results, &TrialResult::sketchydb_hll_first_ms);
    const auto hll_cached = collect(results, &TrialResult::sketchydb_hll_cached_ms);

    const double insert_overhead_ms = mean(sketchy_insert) - mean(duckdb_insert);
    const double per_query_savings_ms = mean(exact_query) - mean(hll_first);
    const double break_even_queries =
        per_query_savings_ms <= 0.0 ? std::numeric_limits<double>::infinity() : insert_overhead_ms / per_query_savings_ms;
    const double relative_error =
        std::abs(results.front().approximate_count - results.front().exact_count) / results.front().exact_count;

    std::cout << "trials=" << kTrials << " rows=" << kRows << " distinct=" << kDistinct << '\n';
    print_metric("duckdb_insert_stream", duckdb_insert);
    print_metric("sketchydb_insert_stream_with_hll", sketchy_insert);
    print_metric("duckdb_count_distinct", exact_query);
    print_metric("sketchydb_hll_first_after_stream_insert", hll_first);
    print_metric("sketchydb_hll_cached", hll_cached);
    std::cout << "insert_overhead_mean_ms=" << insert_overhead_ms << '\n';
    std::cout << "hll_read_speedup_vs_duckdb_mean_x=" << mean(exact_query) / mean(hll_first) << '\n';
    std::cout << "cached_hll_read_speedup_vs_duckdb_mean_x=" << mean(exact_query) / mean(hll_cached) << '\n';
    std::cout << "break_even_approx_queries_after_ingest=" << break_even_queries << '\n';
    std::cout << "exact_count=" << results.front().exact_count
              << " approx_count=" << results.front().approximate_count
              << " relative_error=" << relative_error << '\n';

    return 0;
#endif
}
