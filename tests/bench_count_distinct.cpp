#include "sketchydb.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef SKDB_USE_DUCKDB
#include <duckdb.h>
#endif

namespace {

constexpr const char* kExactSql = "select count(distinct user_id) as exact_count from bench_events";
constexpr const char* kApproxSql = "select approx_count_distinct(user_id, 0.05, 0.90) from bench_events";

struct Options {
    int trials = 5;
    int rows = 500000;
    int distinct_values = 100000;
    int batch_size = 1000;
    std::uint32_t seed = 1337;
};

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

std::string insert_sql_for_batch(const std::vector<int>& values, int batch_start, int batch_size) {
    std::string sql = "insert into bench_events(user_id) values ";
    const int batch_end = std::min<int>(static_cast<int>(values.size()), batch_start + batch_size);
    for (int index = batch_start; index < batch_end; ++index) {
        if (index > batch_start) {
            sql += ", ";
        }
        const int value = values[static_cast<std::size_t>(index)];
        sql += "('user-" + std::to_string(value) + "')";
    }
    return sql;
}

std::vector<int> generate_values(const Options& options, int trial) {
    std::mt19937 generator(options.seed + static_cast<std::uint32_t>(trial));
    std::uniform_int_distribution<int> distribution(0, options.distinct_values - 1);

    std::vector<int> values;
    values.reserve(static_cast<std::size_t>(options.rows));
    for (int index = 0; index < options.rows; ++index) {
        values.push_back(distribution(generator));
    }
    return values;
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

double run_duckdb_insert_baseline(const Options& options, const std::vector<int>& values) {
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
        for (int batch_start = 0; batch_start < options.rows; batch_start += options.batch_size) {
            duckdb_query_or_die(connection, insert_sql_for_batch(values, batch_start, options.batch_size));
        }
    });

    duckdb_disconnect(&connection);
    duckdb_close(&database);
    return insert_ms;
}
#endif

TrialResult run_trial(const Options& options, int trial) {
    TrialResult result;
#ifdef SKDB_USE_DUCKDB
    const auto values = generate_values(options, trial);
    result.duckdb_insert_ms = run_duckdb_insert_baseline(options, values);

    skdb* db = nullptr;
    assert(skdb_open(":memory:", &db) == SKDB_OK);
    exec_or_die(db, "create table bench_events(user_id varchar)");

    result.sketchydb_insert_stream_ms = time_ms([&] {
        for (int batch_start = 0; batch_start < options.rows; batch_start += options.batch_size) {
            exec_or_die(db, insert_sql_for_batch(values, batch_start, options.batch_size));
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

Options parse_options(int argc, char** argv) {
    Options options;
    if (argc > 1) {
        options.trials = std::stoi(argv[1]);
    }
    if (argc > 2) {
        options.rows = std::stoi(argv[2]);
    }
    if (argc > 3) {
        options.distinct_values = std::stoi(argv[3]);
    }
    if (argc > 4) {
        options.batch_size = std::stoi(argv[4]);
    }
    if (argc > 5) {
        options.seed = static_cast<std::uint32_t>(std::stoul(argv[5]));
    }
    if (options.trials < 1 || options.rows < 1 || options.distinct_values < 1 || options.batch_size < 1) {
        throw std::invalid_argument("usage: bench_count_distinct [trials rows distinct batch_size seed]");
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
#ifndef SKDB_USE_DUCKDB
    std::cout << "Skipping perf benchmark; rebuild with SKDB_USE_DUCKDB=1.\n";
    return 0;
#else
    const auto options = parse_options(argc, argv);
    std::vector<TrialResult> results;
    results.reserve(static_cast<std::size_t>(options.trials));
    for (int trial = 0; trial < options.trials; ++trial) {
        results.push_back(run_trial(options, trial));
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

    std::cout << "trials=" << options.trials
              << " rows=" << options.rows
              << " distinct_domain=" << options.distinct_values
              << " batch_size=" << options.batch_size
              << " seed=" << options.seed << '\n';
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
