#include "sketchydb.h"

#include "duckdb_backend.hpp"
#include "frequency_sketch.hpp"
#include "hyperloglog.hpp"
#include "kll_sketch.hpp"
#include "planner.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr const char* kVersion = "0.1.0";
constexpr std::uint64_t kPartitionTargetRows = 65536;
constexpr char kSketchMagic[] = "SKDBHLL2";
constexpr char kKllSketchMagic[] = "SKDBKLL1";
constexpr char kFrequencySketchMagic[] = "SKDBFRQ1";
constexpr double kDefaultStreamingEpsilon = 0.05;
constexpr double kDefaultStreamingConfidence = 0.90;
constexpr std::uint8_t kMaxHllPrecision = 24;

struct HllCacheEntry;
struct KllCacheEntry;
struct FrequencyCacheEntry;

struct HllCallbackState {
    HllCacheEntry* entry = nullptr;
};

struct KllCallbackState {
    KllCacheEntry* entry = nullptr;
};

struct FrequencyCallbackState {
    FrequencyCacheEntry* entry = nullptr;
};

enum class ApproxImplementation {
    HllCountDistinct,
    KllQuantile,
    Frequency,
    TopK,
    Unknown,
};

struct HllCacheEntry {
    double epsilon = 0.0;
    double confidence = 1.0;
    std::uint8_t precision = 0;
    std::uint64_t hash_seed = 0;
    std::string source_table;
    std::string source_column;
    std::vector<sketchydb::HyperLogLog> partitions;
    std::uint64_t active_partition_rows = 0;

    HllCacheEntry(
        double epsilon,
        double confidence,
        std::uint8_t precision,
        std::uint64_t hash_seed,
        std::string table,
        std::string column)
        : epsilon(epsilon),
          confidence(confidence),
          precision(precision),
          hash_seed(hash_seed),
          source_table(std::move(table)),
          source_column(std::move(column)) {
        partitions.emplace_back(precision, hash_seed);
    }
};

struct KllCacheEntry {
    double epsilon = 0.0;
    double confidence = 1.0;
    std::uint32_t capacity = 0;
    std::uint64_t seed = 0;
    std::string source_table;
    std::string source_column;
    sketchydb::KllSketch sketch;

    KllCacheEntry(
        double epsilon,
        double confidence,
        std::uint32_t capacity,
        std::uint64_t seed,
        std::string table,
        std::string column)
        : epsilon(epsilon),
          confidence(confidence),
          capacity(capacity),
          seed(seed),
          source_table(std::move(table)),
          source_column(std::move(column)),
          sketch(capacity, seed) {}

    KllCacheEntry(
        double epsilon,
        double confidence,
        std::uint32_t capacity,
        std::uint64_t seed,
        std::string table,
        std::string column,
        std::uint64_t count,
        std::vector<std::vector<double>> levels)
        : epsilon(epsilon),
          confidence(confidence),
          capacity(capacity),
          seed(seed),
          source_table(std::move(table)),
          source_column(std::move(column)),
          sketch(capacity, seed, count, std::move(levels)) {}
};

struct FrequencyCacheEntry {
    double epsilon = 0.0;
    double confidence = 1.0;
    std::uint32_t width = 0;
    std::uint32_t depth = 0;
    std::uint32_t top_capacity = 0;
    std::uint64_t seed = 0;
    std::string input_sql;
    std::string source_table;
    std::string source_column;
    sketchydb::CountMinSketch count_min;
    sketchydb::SpaceSavingTopK top_k;

    FrequencyCacheEntry(
        double epsilon,
        double confidence,
        std::uint32_t width,
        std::uint32_t depth,
        std::uint32_t top_capacity,
        std::uint64_t seed,
        std::string input_sql,
        std::string table,
        std::string column)
        : epsilon(epsilon),
          confidence(confidence),
          width(width),
          depth(depth),
          top_capacity(top_capacity),
          seed(seed),
          input_sql(std::move(input_sql)),
          source_table(std::move(table)),
          source_column(std::move(column)),
          count_min(width, depth, seed),
          top_k(top_capacity) {}

    FrequencyCacheEntry(
        double epsilon,
        double confidence,
        std::uint32_t width,
        std::uint32_t depth,
        std::uint32_t top_capacity,
        std::uint64_t seed,
        std::string input_sql,
        std::string table,
        std::string column,
        std::vector<std::uint64_t> counters,
        std::vector<sketchydb::TopKItem> items)
        : epsilon(epsilon),
          confidence(confidence),
          width(width),
          depth(depth),
          top_capacity(top_capacity),
          seed(seed),
          input_sql(std::move(input_sql)),
          source_table(std::move(table)),
          source_column(std::move(column)),
          count_min(width, depth, seed, std::move(counters)),
          top_k(top_capacity, std::move(items)) {}
};

}  // namespace

struct skdb {
    std::string filename;
    std::string last_error;
    sketchydb::Planner planner;
    std::unique_ptr<sketchydb::DuckDBBackend> exact_backend;
    std::unordered_map<std::string, HllCacheEntry> hll_cache;
    std::unordered_map<std::string, KllCacheEntry> kll_cache;
    std::unordered_map<std::string, FrequencyCacheEntry> frequency_cache;
    std::uint64_t hash_seed = 0;
};

namespace {

char* duplicate_message(const std::string& message) {
    auto* copy = new (std::nothrow) char[message.size() + 1];
    if (copy == nullptr) {
        return nullptr;
    }

    std::memcpy(copy, message.c_str(), message.size() + 1);
    return copy;
}

int set_error(skdb* db, char** error_message, std::string message) {
    if (db != nullptr) {
        db->last_error = message;
    }

    if (error_message != nullptr) {
        *error_message = duplicate_message(message);
        if (*error_message == nullptr) {
            return SKDB_NOMEM;
        }
    }

    return SKDB_ERROR;
}

int collect_hll_value(void* user_data, int column_count, char** column_values, char** column_names) {
    (void)column_names;

    auto* state = static_cast<HllCallbackState*>(user_data);
    if (state == nullptr || state->entry == nullptr || column_count < 1) {
        return 1;
    }
    if (column_values[0] != nullptr) {
        if (state->entry->active_partition_rows >= kPartitionTargetRows) {
            state->entry->partitions.emplace_back(
                state->entry->precision,
                state->entry->hash_seed);
            state->entry->active_partition_rows = 0;
        }
        state->entry->partitions.back().add(column_values[0]);
        ++state->entry->active_partition_rows;
    }
    return 0;
}

int collect_kll_value(void* user_data, int column_count, char** column_values, char** column_names) {
    (void)column_names;

    auto* state = static_cast<KllCallbackState*>(user_data);
    if (state == nullptr || state->entry == nullptr || column_count < 1) {
        return 1;
    }
    if (column_values[0] == nullptr) {
        return 0;
    }

    char* end = nullptr;
    const double value = std::strtod(column_values[0], &end);
    if (end != column_values[0] && *end == '\0') {
        state->entry->sketch.add(value);
    }
    return 0;
}

int collect_frequency_value(void* user_data, int column_count, char** column_values, char** column_names) {
    (void)column_names;

    auto* state = static_cast<FrequencyCallbackState*>(user_data);
    if (state == nullptr || state->entry == nullptr || column_count < 1) {
        return 1;
    }
    if (column_values[0] != nullptr) {
        state->entry->count_min.add(column_values[0]);
        state->entry->top_k.add(column_values[0]);
    }
    return 0;
}

int emit_single_value(skdb_callback callback, void* user_data, std::string value, const char* column_name) {
    if (callback == nullptr) {
        return SKDB_OK;
    }

    char* values[] = {value.data()};
    char* names[] = {const_cast<char*>(column_name)};
    return callback(user_data, 1, values, names) == 0 ? SKDB_OK : SKDB_ERROR;
}

int emit_top_k(skdb_callback callback, void* user_data, const std::vector<sketchydb::TopKItem>& items) {
    if (callback == nullptr) {
        return SKDB_OK;
    }

    char* names[] = {
        const_cast<char*>("value"),
        const_cast<char*>("estimate"),
        const_cast<char*>("error"),
    };
    for (const auto& item : items) {
        std::string estimate = std::to_string(item.estimate);
        std::string error = std::to_string(item.error);
        char* values[] = {
            const_cast<char*>(item.value.c_str()),
            estimate.data(),
            error.data(),
        };
        if (callback(user_data, 3, values, names) != 0) {
            return SKDB_ERROR;
        }
    }
    return SKDB_OK;
}

ApproxImplementation classify_approx_function(const std::string& function_name) {
    if (function_name == "approx_count_distinct") {
        return ApproxImplementation::HllCountDistinct;
    }
    if (function_name == "approx_25" ||
        function_name == "approx_median" ||
        function_name == "approx_75") {
        return ApproxImplementation::KllQuantile;
    }
    if (function_name == "approx_freq") {
        return ApproxImplementation::Frequency;
    }
    if (function_name == "approx_top_k") {
        return ApproxImplementation::TopK;
    }
    return ApproxImplementation::Unknown;
}

double quantile_probability_for_function(const std::string& function_name) {
    if (function_name == "approx_25") {
        return 0.25;
    }
    if (function_name == "approx_median") {
        return 0.50;
    }
    if (function_name == "approx_75") {
        return 0.75;
    }
    return 0.0;
}

std::uint64_t parse_hash_seed_override() {
    const char* value = std::getenv("SKDB_HASH_SEED");
    if (value == nullptr || *value == '\0') {
        return 0;
    }

    char* end = nullptr;
    const auto parsed = std::strtoull(value, &end, 10);
    return end != value && *end == '\0' ? static_cast<std::uint64_t>(parsed) : 0;
}

std::uint64_t generate_hash_seed() {
    if (const auto override_seed = parse_hash_seed_override(); override_seed != 0) {
        return override_seed;
    }

    std::random_device random_device;
    const auto high = static_cast<std::uint64_t>(random_device()) << 32U;
    const auto low = static_cast<std::uint64_t>(random_device());
    return high ^ low;
}

std::string hll_input_sql(std::string_view table, std::string_view column) {
    return "select " + std::string(column) + " as skdb_hll_value from " + std::string(table);
}

std::string hll_cache_key(
    std::string_view function_name,
    std::string_view input_sql,
    std::uint8_t precision) {
    return std::string(function_name) + "|" + std::string(input_sql) + "|" +
           std::to_string(static_cast<unsigned int>(precision));
}

std::string hll_cache_key(const sketchydb::Plan& plan, std::uint8_t precision) {
    return hll_cache_key(plan.approximate_function, plan.input_sql, precision);
}

std::string kll_cache_key(std::string_view input_sql, std::uint32_t capacity) {
    return "kll|" + std::string(input_sql) + "|" + std::to_string(capacity);
}

std::string kll_input_sql(std::string_view table, std::string_view column) {
    return "select " + std::string(column) + " as skdb_kll_value from " + std::string(table);
}

std::uint32_t compute_top_capacity(std::uint64_t requested_k, double epsilon) {
    const auto by_error = static_cast<std::uint64_t>(std::ceil(1.0 / epsilon));
    const auto capacity = std::max<std::uint64_t>(requested_k, by_error);
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(capacity, 1U << 20U));
}

std::string frequency_cache_key(
    std::string_view input_sql,
    std::uint32_t width,
    std::uint32_t depth,
    std::uint32_t top_capacity) {
    return "freq|" + std::string(input_sql) + "|" + std::to_string(width) + "|" +
           std::to_string(depth) + "|" + std::to_string(top_capacity);
}

std::uint64_t stable_hash(std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hex_hash(std::string_view value) {
    std::ostringstream output;
    output << std::hex << stable_hash(value);
    return output.str();
}

bool persistence_enabled(const skdb* db) {
    return db != nullptr && db->filename != ":memory:";
}

std::filesystem::path sketch_root(const skdb* db) {
    return std::filesystem::path(db->filename).string() + ".sketchydb";
}

std::filesystem::path hash_seed_path(const skdb* db) {
    return sketch_root(db) / "hash_seed";
}

std::filesystem::path sketch_path(const skdb* db, const std::string& cache_key) {
    return sketch_root(db) / (hex_hash(cache_key) + ".hll");
}

std::filesystem::path kll_sketch_path(const skdb* db, const std::string& cache_key) {
    return sketch_root(db) / (hex_hash(cache_key) + ".kll");
}

std::filesystem::path frequency_sketch_path(const skdb* db, const std::string& cache_key) {
    return sketch_root(db) / (hex_hash(cache_key) + ".freq");
}

void save_hash_seed(const skdb* db) {
    if (!persistence_enabled(db) || db->hash_seed == 0) {
        return;
    }

    std::filesystem::create_directories(sketch_root(db));
    std::ofstream output(hash_seed_path(db), std::ios::trunc);
    if (output) {
        output << db->hash_seed << '\n';
    }
}

std::uint64_t load_hash_seed(const skdb* db) {
    if (!persistence_enabled(db)) {
        return 0;
    }

    std::ifstream input(hash_seed_path(db));
    std::uint64_t seed = 0;
    input >> seed;
    return input ? seed : 0;
}

std::uint64_t load_or_create_hash_seed(skdb* db) {
    if (const auto override_seed = parse_hash_seed_override(); override_seed != 0) {
        return override_seed;
    }

    if (const auto persisted_seed = load_hash_seed(db); persisted_seed != 0) {
        return persisted_seed;
    }

    return generate_hash_seed();
}

double estimate_partitioned_hll(const HllCacheEntry& entry) {
    if (entry.partitions.empty()) {
        return 0.0;
    }

    auto merged = entry.partitions.front();
    for (std::size_t index = 1; index < entry.partitions.size(); ++index) {
        merged.merge(entry.partitions[index]);
    }
    return merged.estimate();
}

void write_string(std::ofstream& output, const std::string& value) {
    const std::uint64_t size = value.size();
    output.write(reinterpret_cast<const char*>(&size), sizeof(size));
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

void save_hll_entry(const skdb* db, const std::string& cache_key, const HllCacheEntry& entry) {
    if (!persistence_enabled(db) || entry.partitions.empty()) {
        return;
    }

    std::filesystem::create_directories(sketch_root(db));
    std::ofstream output(sketch_path(db, cache_key), std::ios::binary | std::ios::trunc);
    if (!output) {
        return;
    }

    const std::uint64_t partition_count = entry.partitions.size();
    const std::uint64_t source_table_size = entry.source_table.size();
    const std::uint64_t source_column_size = entry.source_column.size();
    const std::uint64_t register_count = entry.partitions.front().register_count();

    output.write(kSketchMagic, sizeof(kSketchMagic));
    output.write(reinterpret_cast<const char*>(&entry.epsilon), sizeof(entry.epsilon));
    output.write(reinterpret_cast<const char*>(&entry.confidence), sizeof(entry.confidence));
    output.write(reinterpret_cast<const char*>(&entry.precision), sizeof(entry.precision));
    output.write(reinterpret_cast<const char*>(&entry.hash_seed), sizeof(entry.hash_seed));
    output.write(reinterpret_cast<const char*>(&entry.active_partition_rows), sizeof(entry.active_partition_rows));
    output.write(reinterpret_cast<const char*>(&source_table_size), sizeof(source_table_size));
    output.write(entry.source_table.data(), static_cast<std::streamsize>(entry.source_table.size()));
    output.write(reinterpret_cast<const char*>(&source_column_size), sizeof(source_column_size));
    output.write(entry.source_column.data(), static_cast<std::streamsize>(entry.source_column.size()));
    output.write(reinterpret_cast<const char*>(&register_count), sizeof(register_count));
    output.write(reinterpret_cast<const char*>(&partition_count), sizeof(partition_count));

    for (const auto& partition : entry.partitions) {
        output.write(
            reinterpret_cast<const char*>(partition.registers().data()),
            static_cast<std::streamsize>(partition.registers().size()));
    }
}

bool read_string(std::ifstream& input, std::string& value) {
    std::uint64_t size = 0;
    input.read(reinterpret_cast<char*>(&size), sizeof(size));
    if (!input || size > 1024 * 1024) {
        return false;
    }
    value.resize(static_cast<std::size_t>(size));
    input.read(value.data(), static_cast<std::streamsize>(value.size()));
    return static_cast<bool>(input);
}

bool load_hll_entry(const skdb* db, const std::string& cache_key, HllCacheEntry& entry) {
    if (!persistence_enabled(db)) {
        return false;
    }

    std::ifstream input(sketch_path(db, cache_key), std::ios::binary);
    if (!input) {
        return false;
    }

    char magic[sizeof(kSketchMagic)] = {};
    input.read(magic, sizeof(magic));
    if (std::memcmp(magic, kSketchMagic, sizeof(kSketchMagic)) != 0) {
        return false;
    }

    std::uint64_t register_count = 0;
    std::uint64_t partition_count = 0;

    input.read(reinterpret_cast<char*>(&entry.epsilon), sizeof(entry.epsilon));
    input.read(reinterpret_cast<char*>(&entry.confidence), sizeof(entry.confidence));
    input.read(reinterpret_cast<char*>(&entry.precision), sizeof(entry.precision));
    input.read(reinterpret_cast<char*>(&entry.hash_seed), sizeof(entry.hash_seed));
    input.read(reinterpret_cast<char*>(&entry.active_partition_rows), sizeof(entry.active_partition_rows));
    if (!read_string(input, entry.source_table) || !read_string(input, entry.source_column)) {
        return false;
    }
    input.read(reinterpret_cast<char*>(&register_count), sizeof(register_count));
    input.read(reinterpret_cast<char*>(&partition_count), sizeof(partition_count));
    if (!input || partition_count == 0 || partition_count > 100000 || register_count == 0) {
        return false;
    }

    entry.partitions.clear();
    for (std::uint64_t partition = 0; partition < partition_count; ++partition) {
        std::vector<std::uint8_t> registers(static_cast<std::size_t>(register_count));
        input.read(
            reinterpret_cast<char*>(registers.data()),
            static_cast<std::streamsize>(registers.size()));
        if (!input) {
            return false;
        }
        entry.partitions.emplace_back(entry.precision, entry.hash_seed, std::move(registers));
    }

    return true;
}

void save_kll_entry(const skdb* db, const std::string& cache_key, const KllCacheEntry& entry) {
    if (!persistence_enabled(db)) {
        return;
    }

    std::filesystem::create_directories(sketch_root(db));
    std::ofstream output(kll_sketch_path(db, cache_key), std::ios::binary | std::ios::trunc);
    if (!output) {
        return;
    }

    const auto& levels = entry.sketch.levels();
    const std::uint64_t level_count = levels.size();
    output.write(kKllSketchMagic, sizeof(kKllSketchMagic));
    output.write(reinterpret_cast<const char*>(&entry.epsilon), sizeof(entry.epsilon));
    output.write(reinterpret_cast<const char*>(&entry.confidence), sizeof(entry.confidence));
    output.write(reinterpret_cast<const char*>(&entry.capacity), sizeof(entry.capacity));
    output.write(reinterpret_cast<const char*>(&entry.seed), sizeof(entry.seed));
    const auto count = entry.sketch.count();
    output.write(reinterpret_cast<const char*>(&count), sizeof(count));
    write_string(output, entry.source_table);
    write_string(output, entry.source_column);
    output.write(reinterpret_cast<const char*>(&level_count), sizeof(level_count));
    for (const auto& level : levels) {
        const std::uint64_t level_size = level.size();
        output.write(reinterpret_cast<const char*>(&level_size), sizeof(level_size));
        output.write(
            reinterpret_cast<const char*>(level.data()),
            static_cast<std::streamsize>(level.size() * sizeof(double)));
    }
}

bool load_kll_entry(const skdb* db, const std::string& cache_key, KllCacheEntry& entry) {
    if (!persistence_enabled(db)) {
        return false;
    }

    std::ifstream input(kll_sketch_path(db, cache_key), std::ios::binary);
    if (!input) {
        return false;
    }

    char magic[sizeof(kKllSketchMagic)] = {};
    input.read(magic, sizeof(magic));
    if (std::memcmp(magic, kKllSketchMagic, sizeof(kKllSketchMagic)) != 0) {
        return false;
    }

    double epsilon = 0.0;
    double confidence = 1.0;
    std::uint32_t capacity = 0;
    std::uint64_t seed = 0;
    std::uint64_t count = 0;
    std::string source_table;
    std::string source_column;
    std::uint64_t level_count = 0;
    input.read(reinterpret_cast<char*>(&epsilon), sizeof(epsilon));
    input.read(reinterpret_cast<char*>(&confidence), sizeof(confidence));
    input.read(reinterpret_cast<char*>(&capacity), sizeof(capacity));
    input.read(reinterpret_cast<char*>(&seed), sizeof(seed));
    input.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!read_string(input, source_table) || !read_string(input, source_column)) {
        return false;
    }
    input.read(reinterpret_cast<char*>(&level_count), sizeof(level_count));
    if (!input || level_count == 0 || level_count > 128) {
        return false;
    }

    std::vector<std::vector<double>> levels;
    levels.reserve(static_cast<std::size_t>(level_count));
    for (std::uint64_t index = 0; index < level_count; ++index) {
        std::uint64_t level_size = 0;
        input.read(reinterpret_cast<char*>(&level_size), sizeof(level_size));
        if (!input || level_size > 10000000) {
            return false;
        }
        std::vector<double> level(static_cast<std::size_t>(level_size));
        input.read(
            reinterpret_cast<char*>(level.data()),
            static_cast<std::streamsize>(level.size() * sizeof(double)));
        if (!input) {
            return false;
        }
        levels.push_back(std::move(level));
    }

    entry = KllCacheEntry(
        epsilon,
        confidence,
        capacity,
        seed,
        std::move(source_table),
        std::move(source_column),
        count,
        std::move(levels));
    return true;
}

void save_frequency_entry(const skdb* db, const std::string& cache_key, const FrequencyCacheEntry& entry) {
    if (!persistence_enabled(db)) {
        return;
    }

    std::filesystem::create_directories(sketch_root(db));
    std::ofstream output(frequency_sketch_path(db, cache_key), std::ios::binary | std::ios::trunc);
    if (!output) {
        return;
    }

    const auto& counters = entry.count_min.counters();
    const auto top_items = entry.top_k.top_k(entry.top_capacity);
    const std::uint64_t counter_count = counters.size();
    const std::uint64_t item_count = top_items.size();
    output.write(kFrequencySketchMagic, sizeof(kFrequencySketchMagic));
    output.write(reinterpret_cast<const char*>(&entry.epsilon), sizeof(entry.epsilon));
    output.write(reinterpret_cast<const char*>(&entry.confidence), sizeof(entry.confidence));
    output.write(reinterpret_cast<const char*>(&entry.width), sizeof(entry.width));
    output.write(reinterpret_cast<const char*>(&entry.depth), sizeof(entry.depth));
    output.write(reinterpret_cast<const char*>(&entry.top_capacity), sizeof(entry.top_capacity));
    output.write(reinterpret_cast<const char*>(&entry.seed), sizeof(entry.seed));
    write_string(output, entry.input_sql);
    write_string(output, entry.source_table);
    write_string(output, entry.source_column);
    output.write(reinterpret_cast<const char*>(&counter_count), sizeof(counter_count));
    output.write(
        reinterpret_cast<const char*>(counters.data()),
        static_cast<std::streamsize>(counters.size() * sizeof(std::uint64_t)));
    output.write(reinterpret_cast<const char*>(&item_count), sizeof(item_count));
    for (const auto& item : top_items) {
        write_string(output, item.value);
        output.write(reinterpret_cast<const char*>(&item.estimate), sizeof(item.estimate));
        output.write(reinterpret_cast<const char*>(&item.error), sizeof(item.error));
    }
}

bool load_frequency_entry(const skdb* db, const std::string& cache_key, FrequencyCacheEntry& entry) {
    if (!persistence_enabled(db)) {
        return false;
    }

    std::ifstream input(frequency_sketch_path(db, cache_key), std::ios::binary);
    if (!input) {
        return false;
    }

    char magic[sizeof(kFrequencySketchMagic)] = {};
    input.read(magic, sizeof(magic));
    if (std::memcmp(magic, kFrequencySketchMagic, sizeof(kFrequencySketchMagic)) != 0) {
        return false;
    }

    double epsilon = 0.0;
    double confidence = 1.0;
    std::uint32_t width = 0;
    std::uint32_t depth = 0;
    std::uint32_t top_capacity = 0;
    std::uint64_t seed = 0;
    std::string input_sql;
    std::string source_table;
    std::string source_column;
    std::uint64_t counter_count = 0;
    input.read(reinterpret_cast<char*>(&epsilon), sizeof(epsilon));
    input.read(reinterpret_cast<char*>(&confidence), sizeof(confidence));
    input.read(reinterpret_cast<char*>(&width), sizeof(width));
    input.read(reinterpret_cast<char*>(&depth), sizeof(depth));
    input.read(reinterpret_cast<char*>(&top_capacity), sizeof(top_capacity));
    input.read(reinterpret_cast<char*>(&seed), sizeof(seed));
    if (!read_string(input, input_sql) ||
        !read_string(input, source_table) ||
        !read_string(input, source_column)) {
        return false;
    }
    input.read(reinterpret_cast<char*>(&counter_count), sizeof(counter_count));
    if (!input || counter_count == 0 || counter_count > 100000000) {
        return false;
    }
    std::vector<std::uint64_t> counters(static_cast<std::size_t>(counter_count));
    input.read(
        reinterpret_cast<char*>(counters.data()),
        static_cast<std::streamsize>(counters.size() * sizeof(std::uint64_t)));
    if (!input) {
        return false;
    }

    std::uint64_t item_count = 0;
    input.read(reinterpret_cast<char*>(&item_count), sizeof(item_count));
    if (!input || item_count > 1000000) {
        return false;
    }
    std::vector<sketchydb::TopKItem> items;
    items.reserve(static_cast<std::size_t>(item_count));
    for (std::uint64_t index = 0; index < item_count; ++index) {
        sketchydb::TopKItem item;
        if (!read_string(input, item.value)) {
            return false;
        }
        input.read(reinterpret_cast<char*>(&item.estimate), sizeof(item.estimate));
        input.read(reinterpret_cast<char*>(&item.error), sizeof(item.error));
        if (!input) {
            return false;
        }
        items.push_back(std::move(item));
    }

    entry = FrequencyCacheEntry(
        epsilon,
        confidence,
        width,
        depth,
        top_capacity,
        seed,
        std::move(input_sql),
        std::move(source_table),
        std::move(source_column),
        std::move(counters),
        std::move(items));
    return true;
}

void invalidate_persisted_sketches(skdb* db) {
    if (persistence_enabled(db)) {
        const auto seed = db->hash_seed;
        std::filesystem::remove_all(sketch_root(db));
        db->hash_seed = seed;
        save_hash_seed(db);
    }
}

bool add_inserted_values_to_hll_cache(skdb* db, const sketchydb::Plan& plan) {
    if (plan.mutation_kind != sketchydb::MutationKind::InsertValues) {
        return false;
    }

    bool handled_any = false;
    for (auto& [key, entry] : db->hll_cache) {
        (void)key;
        if (entry.source_table != plan.mutation_table || entry.source_column.empty()) {
            continue;
        }

        std::size_t column_index = 0;
        if (!plan.insert_columns.empty()) {
            bool found = false;
            for (std::size_t index = 0; index < plan.insert_columns.size(); ++index) {
                if (plan.insert_columns[index] == entry.source_column) {
                    column_index = index;
                    found = true;
                    break;
                }
            }
            if (!found) {
                continue;
            }
        } else {
            for (const auto& row : plan.insert_rows) {
                if (row.size() != 1) {
                    return false;
                }
            }
            column_index = 0;
        }

        for (const auto& row : plan.insert_rows) {
            if (column_index >= row.size() || row[column_index].empty()) {
                return false;
            }
            if (entry.active_partition_rows >= kPartitionTargetRows) {
                entry.partitions.emplace_back(entry.precision, entry.hash_seed);
                entry.active_partition_rows = 0;
            }
            entry.partitions.back().add(row[column_index]);
            ++entry.active_partition_rows;
        }
        handled_any = true;
    }

    return handled_any;
}

bool find_insert_column_index(const sketchydb::Plan& plan, const std::string& column, std::size_t& column_index) {
    for (std::size_t index = 0; index < plan.insert_columns.size(); ++index) {
        if (plan.insert_columns[index] == column) {
            column_index = index;
            return true;
        }
    }
    return false;
}

bool prewarm_streaming_hll_cache(skdb* db, const sketchydb::Plan& plan) {
    if (plan.mutation_kind != sketchydb::MutationKind::InsertValues || plan.insert_columns.empty()) {
        return false;
    }

    bool prewarmed_any = false;
    const auto& prewarm_columns = plan.has_approx_hint ? plan.approx_hint_columns : plan.insert_columns;
    const double epsilon = plan.has_approx_hint ? plan.approx_hint_epsilon : kDefaultStreamingEpsilon;
    const double confidence = plan.has_approx_hint ? plan.approx_hint_confidence : kDefaultStreamingConfidence;
    for (std::size_t prewarm_index = 0; prewarm_index < prewarm_columns.size(); ++prewarm_index) {
        const auto& column = prewarm_columns[prewarm_index];
        if (column.empty()) {
            continue;
        }
        std::size_t column_index = prewarm_index;
        if (plan.has_approx_hint && !find_insert_column_index(plan, column, column_index)) {
            continue;
        }

        const auto input_sql = hll_input_sql(plan.mutation_table, column);
        const auto default_precision = sketchydb::HyperLogLog::required_precision(
            epsilon,
            confidence);
        const auto cache_key = hll_cache_key(
            "approx_count_distinct",
            input_sql,
            default_precision);

        auto cached = db->hll_cache.find(cache_key);
        if (cached != db->hll_cache.end()) {
            continue;
        }

        auto inserted = db->hll_cache.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(cache_key),
            std::forward_as_tuple(
                epsilon,
                confidence,
                default_precision,
                db->hash_seed,
                plan.mutation_table,
                column));
        cached = inserted.first;

        for (const auto& row : plan.insert_rows) {
            if (column_index >= row.size() || row[column_index].empty()) {
                return false;
            }
            if (cached->second.active_partition_rows >= kPartitionTargetRows) {
                cached->second.partitions.emplace_back(
                    cached->second.precision,
                    cached->second.hash_seed);
                cached->second.active_partition_rows = 0;
            }
            cached->second.partitions.back().add(row[column_index]);
            ++cached->second.active_partition_rows;
        }

        save_hll_entry(db, cache_key, cached->second);
        prewarmed_any = true;
    }

    return prewarmed_any;
}

bool parse_double_value(const std::string& text, double& out_value) {
    char* end = nullptr;
    out_value = std::strtod(text.c_str(), &end);
    return end != text.c_str() && *end == '\0';
}

bool add_inserted_values_to_kll_cache(skdb* db, const sketchydb::Plan& plan) {
    if (plan.mutation_kind != sketchydb::MutationKind::InsertValues) {
        return false;
    }

    bool handled_any = false;
    for (auto& [key, entry] : db->kll_cache) {
        (void)key;
        if (entry.source_table != plan.mutation_table || entry.source_column.empty()) {
            continue;
        }

        std::size_t column_index = 0;
        if (!plan.insert_columns.empty()) {
            bool found = false;
            for (std::size_t index = 0; index < plan.insert_columns.size(); ++index) {
                if (plan.insert_columns[index] == entry.source_column) {
                    column_index = index;
                    found = true;
                    break;
                }
            }
            if (!found) {
                continue;
            }
        } else {
            for (const auto& row : plan.insert_rows) {
                if (row.size() != 1) {
                    return false;
                }
            }
            column_index = 0;
        }

        for (const auto& row : plan.insert_rows) {
            double value = 0.0;
            if (column_index >= row.size() || !parse_double_value(row[column_index], value)) {
                return false;
            }
            entry.sketch.add(value);
        }
        handled_any = true;
    }

    return handled_any;
}

bool prewarm_streaming_kll_cache(skdb* db, const sketchydb::Plan& plan) {
    if (plan.mutation_kind != sketchydb::MutationKind::InsertValues || plan.insert_columns.empty()) {
        return false;
    }

    bool prewarmed_any = false;
    const auto& prewarm_columns = plan.has_approx_hint ? plan.approx_hint_columns : plan.insert_columns;
    const double epsilon = plan.has_approx_hint ? plan.approx_hint_epsilon : kDefaultStreamingEpsilon;
    const double confidence = plan.has_approx_hint ? plan.approx_hint_confidence : kDefaultStreamingConfidence;
    const auto default_capacity = sketchydb::KllSketch::required_capacity(
        epsilon,
        confidence);

    for (std::size_t prewarm_index = 0; prewarm_index < prewarm_columns.size(); ++prewarm_index) {
        const auto& column = prewarm_columns[prewarm_index];
        if (column.empty()) {
            continue;
        }
        std::size_t column_index = prewarm_index;
        if (plan.has_approx_hint && !find_insert_column_index(plan, column, column_index)) {
            continue;
        }

        const auto input_sql = kll_input_sql(plan.mutation_table, column);
        const auto cache_key = kll_cache_key(input_sql, default_capacity);
        if (db->kll_cache.find(cache_key) != db->kll_cache.end()) {
            continue;
        }

        auto inserted = db->kll_cache.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(cache_key),
            std::forward_as_tuple(
                epsilon,
                confidence,
                default_capacity,
                db->hash_seed,
                plan.mutation_table,
                column));

        bool saw_numeric_value = false;
        for (const auto& row : plan.insert_rows) {
            double value = 0.0;
            if (column_index < row.size() && parse_double_value(row[column_index], value)) {
                inserted.first->second.sketch.add(value);
                saw_numeric_value = true;
            }
        }

        if (saw_numeric_value) {
            prewarmed_any = true;
        } else {
            db->kll_cache.erase(inserted.first);
        }
    }

    return prewarmed_any;
}

void add_value_to_frequency_entry(FrequencyCacheEntry& entry, std::string_view value) {
    entry.count_min.add(value);
    entry.top_k.add(value);
}

bool add_inserted_values_to_frequency_cache(skdb* db, const sketchydb::Plan& plan) {
    if (plan.mutation_kind != sketchydb::MutationKind::InsertValues) {
        return false;
    }

    bool handled_any = false;
    for (auto& [key, entry] : db->frequency_cache) {
        (void)key;
        if (entry.source_table != plan.mutation_table || entry.source_column.empty()) {
            continue;
        }

        std::size_t column_index = 0;
        if (!plan.insert_columns.empty()) {
            bool found = false;
            for (std::size_t index = 0; index < plan.insert_columns.size(); ++index) {
                if (plan.insert_columns[index] == entry.source_column) {
                    column_index = index;
                    found = true;
                    break;
                }
            }
            if (!found) {
                continue;
            }
        } else {
            for (const auto& row : plan.insert_rows) {
                if (row.size() != 1) {
                    return false;
                }
            }
            column_index = 0;
        }

        for (const auto& row : plan.insert_rows) {
            if (column_index >= row.size() || row[column_index].empty()) {
                return false;
            }
            add_value_to_frequency_entry(entry, row[column_index]);
        }
        handled_any = true;
    }

    return handled_any;
}

bool prewarm_streaming_frequency_cache(skdb* db, const sketchydb::Plan& plan) {
    if (plan.mutation_kind != sketchydb::MutationKind::InsertValues || plan.insert_columns.empty()) {
        return false;
    }

    bool prewarmed_any = false;
    const auto& prewarm_columns = plan.has_approx_hint ? plan.approx_hint_columns : plan.insert_columns;
    const double epsilon = plan.has_approx_hint ? plan.approx_hint_epsilon : kDefaultStreamingEpsilon;
    const double confidence = plan.has_approx_hint ? plan.approx_hint_confidence : kDefaultStreamingConfidence;
    const auto default_width = sketchydb::CountMinSketch::required_width(epsilon);
    const auto default_depth = sketchydb::CountMinSketch::required_depth(confidence);
    const auto default_top_capacity = compute_top_capacity(10, epsilon);

    for (std::size_t prewarm_index = 0; prewarm_index < prewarm_columns.size(); ++prewarm_index) {
        const auto& column = prewarm_columns[prewarm_index];
        if (column.empty()) {
            continue;
        }
        std::size_t column_index = prewarm_index;
        if (plan.has_approx_hint && !find_insert_column_index(plan, column, column_index)) {
            continue;
        }

        const auto input_sql = hll_input_sql(plan.mutation_table, column);
        const auto cache_key = frequency_cache_key(input_sql, default_width, default_depth, default_top_capacity);
        if (db->frequency_cache.find(cache_key) != db->frequency_cache.end()) {
            continue;
        }

        auto inserted = db->frequency_cache.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(cache_key),
            std::forward_as_tuple(
                epsilon,
                confidence,
                default_width,
                default_depth,
                default_top_capacity,
                db->hash_seed,
                input_sql,
                plan.mutation_table,
                column));

        bool saw_value = false;
        for (const auto& row : plan.insert_rows) {
            if (column_index < row.size() && !row[column_index].empty()) {
                add_value_to_frequency_entry(inserted.first->second, row[column_index]);
                saw_value = true;
            }
        }

        if (saw_value) {
            prewarmed_any = true;
        } else {
            db->frequency_cache.erase(inserted.first);
        }
    }

    return prewarmed_any;
}

std::string hll_cache_key_for_precision(const sketchydb::Plan& plan, std::uint8_t precision) {
    return hll_cache_key(plan, precision);
}

std::pair<std::string, HllCacheEntry*> find_sufficient_hll_cache(
    skdb* db,
    const sketchydb::Plan& plan,
    std::uint8_t required_precision) {
    for (std::uint8_t precision = required_precision; precision <= kMaxHllPrecision; ++precision) {
        auto key = hll_cache_key_for_precision(plan, precision);
        auto found = db->hll_cache.find(key);
        if (found != db->hll_cache.end() && found->second.hash_seed == db->hash_seed) {
            return {std::move(key), &found->second};
        }
    }

    return {"", nullptr};
}

std::pair<std::string, HllCacheEntry*> load_sufficient_hll_cache(
    skdb* db,
    const sketchydb::Plan& plan,
    std::uint8_t required_precision) {
    for (std::uint8_t precision = required_precision; precision <= kMaxHllPrecision; ++precision) {
        auto key = hll_cache_key_for_precision(plan, precision);
        auto inserted = db->hll_cache.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(key),
            std::forward_as_tuple(
                plan.epsilon,
                plan.confidence,
                precision,
                db->hash_seed,
                plan.source_table,
                plan.source_column));

        if (load_hll_entry(db, key, inserted.first->second) &&
            inserted.first->second.hash_seed == db->hash_seed &&
            inserted.first->second.precision >= required_precision) {
            return {std::move(key), &inserted.first->second};
        }

        db->hll_cache.erase(inserted.first);
    }

    return {"", nullptr};
}

std::pair<std::string, KllCacheEntry*> find_sufficient_kll_cache(
    skdb* db,
    const sketchydb::Plan& plan,
    std::uint32_t required_capacity) {
    std::pair<std::string, KllCacheEntry*> best{"", nullptr};
    for (auto& [key, entry] : db->kll_cache) {
        if (entry.source_table == plan.source_table &&
            entry.source_column == plan.source_column &&
            entry.seed == db->hash_seed &&
            entry.capacity >= required_capacity) {
            if (best.second == nullptr || entry.capacity < best.second->capacity) {
                best = {key, &entry};
            }
        }
    }
    return best;
}

std::pair<std::string, KllCacheEntry*> load_sufficient_kll_cache(
    skdb* db,
    const sketchydb::Plan& plan,
    std::uint32_t required_capacity) {
    const auto key = kll_cache_key(plan.input_sql, required_capacity);
    auto inserted = db->kll_cache.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(key),
        std::forward_as_tuple(
            plan.epsilon,
            plan.confidence,
            required_capacity,
            db->hash_seed,
            plan.source_table,
            plan.source_column));
    if (load_kll_entry(db, key, inserted.first->second) &&
        inserted.first->second.seed == db->hash_seed &&
        inserted.first->second.capacity >= required_capacity) {
        return {key, &inserted.first->second};
    }
    db->kll_cache.erase(inserted.first);
    return {"", nullptr};
}

std::string kll_cache_key_for_capacity(const sketchydb::Plan& plan, std::uint32_t capacity) {
    return kll_cache_key(plan.input_sql, capacity);
}

std::pair<std::string, FrequencyCacheEntry*> find_sufficient_frequency_cache(
    skdb* db,
    const sketchydb::Plan& plan,
    std::uint32_t required_width,
    std::uint32_t required_depth,
    std::uint32_t required_top_capacity) {
    std::pair<std::string, FrequencyCacheEntry*> best{"", nullptr};
    for (auto& [key, entry] : db->frequency_cache) {
        if (entry.input_sql == plan.input_sql &&
            entry.seed == db->hash_seed &&
            entry.width >= required_width &&
            entry.depth >= required_depth &&
            entry.top_capacity >= required_top_capacity) {
            if (best.second == nullptr ||
                (entry.width * entry.depth + entry.top_capacity) <
                    (best.second->width * best.second->depth + best.second->top_capacity)) {
                best = {key, &entry};
            }
        }
    }
    return best;
}

std::string frequency_cache_key_for_requirements(
    const sketchydb::Plan& plan,
    std::uint32_t width,
    std::uint32_t depth,
    std::uint32_t top_capacity) {
    return frequency_cache_key(plan.input_sql, width, depth, top_capacity);
}

std::pair<std::string, FrequencyCacheEntry*> load_sufficient_frequency_cache(
    skdb* db,
    const sketchydb::Plan& plan,
    std::uint32_t required_width,
    std::uint32_t required_depth,
    std::uint32_t required_top_capacity) {
    const auto key = frequency_cache_key_for_requirements(plan, required_width, required_depth, required_top_capacity);
    auto inserted = db->frequency_cache.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(key),
        std::forward_as_tuple(
            plan.epsilon,
            plan.confidence,
            required_width,
            required_depth,
            required_top_capacity,
            db->hash_seed,
            plan.input_sql,
            plan.source_table,
            plan.source_column));
    if (load_frequency_entry(db, key, inserted.first->second) &&
        inserted.first->second.seed == db->hash_seed &&
        inserted.first->second.width >= required_width &&
        inserted.first->second.depth >= required_depth &&
        inserted.first->second.top_capacity >= required_top_capacity) {
        return {key, &inserted.first->second};
    }
    db->frequency_cache.erase(inserted.first);
    return {"", nullptr};
}

std::uint64_t hll_cache_entry_memory_bytes(const HllCacheEntry& entry) {
    auto bytes = static_cast<std::uint64_t>(sizeof(HllCacheEntry));
    bytes += static_cast<std::uint64_t>(entry.source_table.capacity());
    bytes += static_cast<std::uint64_t>(entry.source_column.capacity());
    bytes += static_cast<std::uint64_t>(entry.partitions.capacity() * sizeof(sketchydb::HyperLogLog));

    for (const auto& partition : entry.partitions) {
        bytes += static_cast<std::uint64_t>(partition.registers().capacity());
    }

    return bytes;
}

std::uint64_t kll_cache_entry_memory_bytes(const KllCacheEntry& entry) {
    auto bytes = static_cast<std::uint64_t>(sizeof(KllCacheEntry));
    bytes += static_cast<std::uint64_t>(entry.source_table.capacity());
    bytes += static_cast<std::uint64_t>(entry.source_column.capacity());
    bytes += entry.sketch.memory_bytes();
    return bytes;
}

std::uint64_t frequency_cache_entry_memory_bytes(const FrequencyCacheEntry& entry) {
    auto bytes = static_cast<std::uint64_t>(sizeof(FrequencyCacheEntry));
    bytes += static_cast<std::uint64_t>(entry.input_sql.capacity());
    bytes += static_cast<std::uint64_t>(entry.source_table.capacity());
    bytes += static_cast<std::uint64_t>(entry.source_column.capacity());
    bytes += entry.count_min.memory_bytes();
    bytes += entry.top_k.memory_bytes();
    return bytes;
}

}  // namespace

int skdb_open(const char* filename, skdb** out_db) {
    if (out_db == nullptr) {
        return SKDB_MISUSE;
    }

    *out_db = nullptr;

    auto* db = new (std::nothrow) skdb;
    if (db == nullptr) {
        return SKDB_NOMEM;
    }

    db->filename = filename == nullptr ? ":memory:" : filename;
    db->hash_seed = load_or_create_hash_seed(db);
    save_hash_seed(db);
    try {
        db->exact_backend = std::make_unique<sketchydb::DuckDBBackend>(db->filename);
    } catch (const std::exception& exception) {
        delete db;
        return SKDB_ERROR;
    }

    *out_db = db;
    return SKDB_OK;
}

int skdb_close(skdb* db) {
    delete db;
    return SKDB_OK;
}

int skdb_exec(
    skdb* db,
    const char* sql,
    skdb_callback callback,
    void* user_data,
    char** error_message) {
    if (error_message != nullptr) {
        *error_message = nullptr;
    }

    if (db == nullptr || sql == nullptr) {
        return SKDB_MISUSE;
    }

    auto plan = db->planner.plan(sql);
    if (plan.mode == sketchydb::ExecutionMode::Approximate) {
        if (!plan.error_message.empty()) {
            return set_error(db, error_message, plan.error_message);
        }
        const auto implementation = classify_approx_function(plan.approximate_function);
        if (implementation == ApproxImplementation::Unknown) {
            return set_error(
                db,
                error_message,
                plan.approximate_function + " is not a supported SketchyDB approximate function yet");
        }
        if (implementation == ApproxImplementation::Frequency ||
            implementation == ApproxImplementation::TopK) {
            const auto required_width = sketchydb::CountMinSketch::required_width(plan.epsilon);
            const auto required_depth = sketchydb::CountMinSketch::required_depth(plan.confidence);
            const auto required_top_capacity = implementation == ApproxImplementation::TopK
                                                   ? compute_top_capacity(plan.top_k, plan.epsilon)
                                                   : 0;
            auto [cache_key, cached] = find_sufficient_frequency_cache(
                db,
                plan,
                required_width,
                required_depth,
                required_top_capacity);
            if (cached == nullptr) {
                auto loaded = load_sufficient_frequency_cache(
                    db,
                    plan,
                    required_width,
                    required_depth,
                    required_top_capacity);
                cache_key = std::move(loaded.first);
                cached = loaded.second;
            }
            if (cached == nullptr) {
                cache_key = frequency_cache_key_for_requirements(
                    plan,
                    required_width,
                    required_depth,
                    required_top_capacity);
                auto inserted = db->frequency_cache.emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(cache_key),
                    std::forward_as_tuple(
                        plan.epsilon,
                        plan.confidence,
                        required_width,
                        required_depth,
                        required_top_capacity,
                        db->hash_seed,
                        plan.input_sql,
                        plan.source_table,
                        plan.source_column));
                cached = &inserted.first->second;

                FrequencyCallbackState state{.entry = cached};
                std::string error;
                int rc = db->exact_backend->exec(
                    plan.input_sql.c_str(),
                    collect_frequency_value,
                    &state,
                    error);
                if (rc != SKDB_OK) {
                    db->frequency_cache.erase(cache_key);
                    return set_error(db, error_message, error);
                }
                save_frequency_entry(db, cache_key, *cached);
            }

            int rc = SKDB_OK;
            if (implementation == ApproxImplementation::Frequency) {
                rc = emit_single_value(
                    callback,
                    user_data,
                    std::to_string(cached->count_min.estimate(plan.target_value)),
                    "approx_freq");
            } else {
                rc = emit_top_k(callback, user_data, cached->top_k.top_k(static_cast<std::uint32_t>(plan.top_k)));
            }
            if (rc != SKDB_OK) {
                return set_error(db, error_message, "query aborted by callback");
            }

            db->last_error.clear();
            return SKDB_OK;
        }
        if (implementation == ApproxImplementation::KllQuantile) {
            const auto required_capacity =
                sketchydb::KllSketch::required_capacity(plan.epsilon, plan.confidence);
            auto [cache_key, cached] = find_sufficient_kll_cache(db, plan, required_capacity);
            if (cached == nullptr) {
                auto loaded = load_sufficient_kll_cache(db, plan, required_capacity);
                cache_key = std::move(loaded.first);
                cached = loaded.second;
            }
            if (cached == nullptr) {
                cache_key = kll_cache_key_for_capacity(plan, required_capacity);
                auto inserted = db->kll_cache.emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(cache_key),
                    std::forward_as_tuple(
                        plan.epsilon,
                        plan.confidence,
                        required_capacity,
                        db->hash_seed,
                        plan.source_table,
                        plan.source_column));
                cached = &inserted.first->second;

                KllCallbackState state{.entry = cached};
                std::string error;
                int rc = db->exact_backend->exec(
                    plan.input_sql.c_str(),
                    collect_kll_value,
                    &state,
                    error);
                if (rc != SKDB_OK) {
                    db->kll_cache.erase(cache_key);
                    return set_error(db, error_message, error);
                }
                save_kll_entry(db, cache_key, *cached);
            }

            int rc = emit_single_value(
                callback,
                user_data,
                std::to_string(cached->sketch.quantile(quantile_probability_for_function(plan.approximate_function))),
                plan.approximate_function.c_str());
            if (rc != SKDB_OK) {
                return set_error(db, error_message, "query aborted by callback");
            }

            db->last_error.clear();
            return SKDB_OK;
        }

        const auto required_precision =
            sketchydb::HyperLogLog::required_precision(plan.epsilon, plan.confidence);
        auto [cache_key, cached] = find_sufficient_hll_cache(db, plan, required_precision);
        if (cached == nullptr) {
            auto loaded = load_sufficient_hll_cache(db, plan, required_precision);
            cache_key = std::move(loaded.first);
            cached = loaded.second;
        }
        if (cached == nullptr) {
            cache_key = hll_cache_key_for_precision(plan, required_precision);
            auto inserted = db->hll_cache.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(cache_key),
                std::forward_as_tuple(
                    plan.epsilon,
                    plan.confidence,
                    required_precision,
                    db->hash_seed,
                    plan.source_table,
                    plan.source_column));
            cached = &inserted.first->second;

            HllCallbackState state{.entry = cached};
            std::string error;
            int rc = db->exact_backend->exec(
                plan.input_sql.c_str(),
                collect_hll_value,
                &state,
                error);
            if (rc != SKDB_OK) {
                db->hll_cache.erase(cache_key);
                return set_error(db, error_message, error);
            }
            save_hll_entry(db, cache_key, *cached);
        }

        int rc = emit_single_value(
            callback,
            user_data,
            std::to_string(estimate_partitioned_hll(*cached)),
            "approx_count_distinct");
        if (rc != SKDB_OK) {
            return set_error(db, error_message, "query aborted by callback");
        }

        db->last_error.clear();
        return SKDB_OK;
    }

    std::string error;
    const std::string& sql_to_execute = plan.sql_to_execute.empty() ? std::string() : plan.sql_to_execute;
    const char* exact_sql = plan.sql_to_execute.empty() ? sql : sql_to_execute.c_str();
    int rc = db->exact_backend->exec(exact_sql, callback, user_data, error);
    if (rc != SKDB_OK) {
        return set_error(db, error_message, error);
    }
    if (plan.mutation_kind == sketchydb::MutationKind::InsertValues) {
        const bool updated_existing = add_inserted_values_to_hll_cache(db, plan);
        const bool prewarmed = prewarm_streaming_hll_cache(db, plan);
        const bool updated_existing_kll = add_inserted_values_to_kll_cache(db, plan);
        const bool prewarmed_kll = prewarm_streaming_kll_cache(db, plan);
        const bool updated_existing_frequency = add_inserted_values_to_frequency_cache(db, plan);
        const bool prewarmed_frequency = prewarm_streaming_frequency_cache(db, plan);
        for (const auto& [cache_key, entry] : db->hll_cache) {
            save_hll_entry(db, cache_key, entry);
        }
        for (const auto& [cache_key, entry] : db->kll_cache) {
            save_kll_entry(db, cache_key, entry);
        }
        for (const auto& [cache_key, entry] : db->frequency_cache) {
            save_frequency_entry(db, cache_key, entry);
        }
        if (updated_existing || prewarmed ||
            updated_existing_kll || prewarmed_kll ||
            updated_existing_frequency || prewarmed_frequency) {
            db->last_error.clear();
            return SKDB_OK;
        }
    }
    if (plan.invalidates_sketches) {
        db->hll_cache.clear();
        db->kll_cache.clear();
        db->frequency_cache.clear();
        invalidate_persisted_sketches(db);
    }

    db->last_error.clear();
    return SKDB_OK;
}

const char* skdb_errmsg(skdb* db) {
    if (db == nullptr) {
        return "bad database handle";
    }
    if (db->last_error.empty()) {
        return "not an error";
    }
    return db->last_error.c_str();
}

std::uint64_t skdb_approx_memory_bytes(skdb* db) {
    if (db == nullptr) {
        return 0;
    }

    auto bytes = static_cast<std::uint64_t>(db->hll_cache.bucket_count() * sizeof(void*));
    for (const auto& [cache_key, entry] : db->hll_cache) {
        bytes += static_cast<std::uint64_t>(sizeof(void*) * 2);
        bytes += static_cast<std::uint64_t>(sizeof(std::string) + cache_key.capacity());
        bytes += hll_cache_entry_memory_bytes(entry);
    }
    bytes += static_cast<std::uint64_t>(db->kll_cache.bucket_count() * sizeof(void*));
    for (const auto& [cache_key, entry] : db->kll_cache) {
        bytes += static_cast<std::uint64_t>(sizeof(void*) * 2);
        bytes += static_cast<std::uint64_t>(sizeof(std::string) + cache_key.capacity());
        bytes += kll_cache_entry_memory_bytes(entry);
    }
    bytes += static_cast<std::uint64_t>(db->frequency_cache.bucket_count() * sizeof(void*));
    for (const auto& [cache_key, entry] : db->frequency_cache) {
        bytes += static_cast<std::uint64_t>(sizeof(void*) * 2);
        bytes += static_cast<std::uint64_t>(sizeof(std::string) + cache_key.capacity());
        bytes += frequency_cache_entry_memory_bytes(entry);
    }

    return bytes;
}

void skdb_free(void* ptr) {
    delete[] static_cast<char*>(ptr);
}

const char* skdb_libversion(void) {
    return kVersion;
}
