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

constexpr const char* kExactFreqSql = "select count(*) as exact_frequency from bench_events where user_id = 'hot-0'";
constexpr const char* kApproxFreqSql = "select approx_freq(user_id, 'hot-0', 0.05, 0.90) from bench_events";
constexpr const char* kExactTopKSql =
    "select user_id, count(*) as frequency from bench_events group by user_id order by frequency desc, user_id limit 10";
constexpr const char* kApproxTopKSql = "select approx_top_k(user_id, 10, 0.05, 0.90) from bench_events";

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

struct RowCount {
    int count = 0;
};

struct TrialResult {
    double duckdb_insert_ms = 0.0;
    double sketchydb_insert_stream_ms = 0.0;
    double duckdb_exact_freq_ms = 0.0;
    double sketchydb_freq_ms = 0.0;
    double duckdb_exact_topk_ms = 0.0;
    double sketchydb_topk_ms = 0.0;
    double sketchydb_approx_memory_bytes = 0.0;
    double exact_frequency = 0.0;
    double approximate_frequency = 0.0;
    double approximate_topk_rows = 0.0;
};

int capture_single_value(void* user_data, int column_count, char** column_values, char** column_names) {
    (void)column_names;

    auto* captured = static_cast<SingleValue*>(user_data);
    assert(column_count == 1);
    captured->value = column_values[0] == nullptr ? "" : column_values[0];
    return 0;
}

int count_rows(void* user_data, int column_count, char** column_values, char** column_names) {
    (void)column_values;
    (void)column_names;

    auto* captured = static_cast<RowCount*>(user_data);
    assert(column_count >= 1);
    ++captured->count;
    return 0;
}

template <typename Function>
double time_ms(Function function) {
    const auto start = std::chrono::steady_clock::now();
    function();
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

std::vector<std::string> generate_values(const Options& options, int trial) {
    std::mt19937 generator(options.seed + static_cast<std::uint32_t>(trial));
    std::uniform_int_distribution<int> tail_distribution(0, options.distinct_values - 1);
    std::uniform_real_distribution<double> probability(0.0, 1.0);

    std::vector<std::string> values;
    values.reserve(static_cast<std::size_t>(options.rows));
    for (int index = 0; index < options.rows; ++index) {
        const double draw = probability(generator);
        if (draw < 0.20) {
            values.push_back("hot-0");
        } else if (draw < 0.32) {
            values.push_back("hot-1");
        } else if (draw < 0.40) {
            values.push_back("hot-2");
        } else {
            values.push_back("user-" + std::to_string(tail_distribution(generator)));
        }
    }
    return values;
}

std::string insert_sql_for_batch(const std::vector<std::string>& values, int batch_start, int batch_size) {
    std::string sql = "insert into bench_events(user_id) values ";
    const int batch_end = std::min<int>(static_cast<int>(values.size()), batch_start + batch_size);
    for (int index = batch_start; index < batch_end; ++index) {
        if (index > batch_start) {
            sql += ", ";
        }
        sql += "('" + values[static_cast<std::size_t>(index)] + "')";
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

int query_row_count(skdb* db, const char* sql) {
    RowCount rows;
    char* error_message = nullptr;
    const int rc = skdb_exec(db, sql, count_rows, &rows, &error_message);
    if (rc != SKDB_OK) {
        std::cerr << "sketchydb error: " << (error_message == nullptr ? skdb_errmsg(db) : error_message) << '\n';
        skdb_free(error_message);
        std::abort();
    }
    return rows.count;
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

double run_duckdb_insert_baseline(const Options& options, const std::vector<std::string>& values) {
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

    SingleValue exact_frequency;
    result.duckdb_exact_freq_ms = time_ms([&] {
        exact_frequency = query_single_value(db, kExactFreqSql);
    });

    SingleValue approximate_frequency;
    result.sketchydb_freq_ms = time_ms([&] {
        approximate_frequency = query_single_value(db, kApproxFreqSql);
    });

    int exact_topk_rows = 0;
    result.duckdb_exact_topk_ms = time_ms([&] {
        exact_topk_rows = query_row_count(db, kExactTopKSql);
    });

    int approximate_topk_rows = 0;
    result.sketchydb_topk_ms = time_ms([&] {
        approximate_topk_rows = query_row_count(db, kApproxTopKSql);
    });

    result.exact_frequency = std::stod(exact_frequency.value);
    result.approximate_frequency = std::stod(approximate_frequency.value);
    result.approximate_topk_rows = approximate_topk_rows;
    result.sketchydb_approx_memory_bytes = static_cast<double>(skdb_approx_memory_bytes(db));
    assert(exact_topk_rows == 10);

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
        throw std::invalid_argument("usage: bench_frequency [trials rows distinct batch_size seed]");
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
#ifndef SKDB_USE_DUCKDB
    std::cout << "Skipping frequency perf benchmark; rebuild with SKDB_USE_DUCKDB=1.\n";
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
    const auto exact_freq = collect(results, &TrialResult::duckdb_exact_freq_ms);
    const auto approx_freq = collect(results, &TrialResult::sketchydb_freq_ms);
    const auto exact_topk = collect(results, &TrialResult::duckdb_exact_topk_ms);
    const auto approx_topk = collect(results, &TrialResult::sketchydb_topk_ms);
    const auto approx_memory = collect(results, &TrialResult::sketchydb_approx_memory_bytes);

    const double insert_overhead_ms = mean(sketchy_insert) - mean(duckdb_insert);
    const double freq_savings_ms = mean(exact_freq) - mean(approx_freq);
    const double topk_savings_ms = mean(exact_topk) - mean(approx_topk);
    const double freq_break_even =
        freq_savings_ms <= 0.0 ? std::numeric_limits<double>::infinity() : insert_overhead_ms / freq_savings_ms;
    const double topk_break_even =
        topk_savings_ms <= 0.0 ? std::numeric_limits<double>::infinity() : insert_overhead_ms / topk_savings_ms;
    const double freq_relative_error =
        std::abs(results.front().approximate_frequency - results.front().exact_frequency) /
        std::max(1.0, results.front().exact_frequency);

    std::cout << "trials=" << options.trials
              << " rows=" << options.rows
              << " distinct_domain=" << options.distinct_values
              << " batch_size=" << options.batch_size
              << " seed=" << options.seed << '\n';
    print_metric("duckdb_insert_stream", duckdb_insert);
    print_metric("sketchydb_insert_stream_with_frequency", sketchy_insert);
    print_metric("duckdb_exact_frequency", exact_freq);
    print_metric("sketchydb_approx_frequency", approx_freq);
    print_metric("duckdb_exact_top_k", exact_topk);
    print_metric("sketchydb_approx_top_k", approx_topk);
    std::cout << "insert_overhead_mean_ms=" << insert_overhead_ms << '\n';
    std::cout << "freq_read_speedup_vs_duckdb_mean_x=" << mean(exact_freq) / mean(approx_freq) << '\n';
    std::cout << "topk_read_speedup_vs_duckdb_mean_x=" << mean(exact_topk) / mean(approx_topk) << '\n';
    std::cout << "freq_break_even_approx_queries_after_ingest=" << freq_break_even << '\n';
    std::cout << "topk_break_even_approx_queries_after_ingest=" << topk_break_even << '\n';
    std::cout << "sketchydb_approx_memory_mean_bytes=" << mean(approx_memory)
              << " median_bytes=" << median(approx_memory)
              << " mean_mib=" << mean(approx_memory) / (1024.0 * 1024.0) << '\n';
    std::cout << "exact_frequency=" << results.front().exact_frequency
              << " approx_frequency=" << results.front().approximate_frequency
              << " freq_relative_error=" << freq_relative_error
              << " approx_topk_rows=" << results.front().approximate_topk_rows << '\n';

    return 0;
#endif
}
