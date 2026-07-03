#include "sketchydb.h"

#include "duckdb_backend.hpp"
#include "hyperloglog.hpp"
#include "planner.hpp"

#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr const char* kVersion = "0.1.0";
constexpr std::uint64_t kPartitionTargetRows = 65536;
constexpr char kSketchMagic[] = "SKDBHLL1";
constexpr double kDefaultStreamingEpsilon = 0.05;
constexpr double kDefaultStreamingConfidence = 0.90;

struct HllCacheEntry;

struct HllCallbackState {
    HllCacheEntry* entry = nullptr;
};

struct HllCacheEntry {
    double epsilon = 0.0;
    double confidence = 1.0;
    std::string source_table;
    std::string source_column;
    std::vector<sketchydb::HyperLogLog> partitions;
    std::uint64_t active_partition_rows = 0;

    HllCacheEntry(double epsilon, double confidence, std::string table, std::string column)
        : epsilon(epsilon),
          confidence(confidence),
          source_table(std::move(table)),
          source_column(std::move(column)) {
        partitions.emplace_back(epsilon, confidence);
    }
};

}  // namespace

struct skdb {
    std::string filename;
    std::string last_error;
    sketchydb::Planner planner;
    std::unique_ptr<sketchydb::DuckDBBackend> exact_backend;
    std::unordered_map<std::string, HllCacheEntry> hll_cache;
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
            state->entry->partitions.emplace_back(state->entry->epsilon, state->entry->confidence);
            state->entry->active_partition_rows = 0;
        }
        state->entry->partitions.back().add(column_values[0]);
        ++state->entry->active_partition_rows;
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

std::string hll_cache_key(const sketchydb::Plan& plan) {
    return plan.approximate_function + "|" + plan.input_sql + "|" +
           std::to_string(plan.epsilon) + "|" + std::to_string(plan.confidence);
}

std::string hll_input_sql(std::string_view table, std::string_view column) {
    return "select " + std::string(column) + " as skdb_hll_value from " + std::string(table);
}

std::string hll_cache_key(
    std::string_view function_name,
    std::string_view input_sql,
    double epsilon,
    double confidence) {
    return std::string(function_name) + "|" + std::string(input_sql) + "|" +
           std::to_string(epsilon) + "|" + std::to_string(confidence);
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

std::filesystem::path sketch_path(const skdb* db, const std::string& cache_key) {
    return sketch_root(db) / (hex_hash(cache_key) + ".hll");
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
    const auto precision = entry.partitions.front().precision();
    const std::uint64_t register_count = entry.partitions.front().register_count();

    output.write(kSketchMagic, sizeof(kSketchMagic));
    output.write(reinterpret_cast<const char*>(&entry.epsilon), sizeof(entry.epsilon));
    output.write(reinterpret_cast<const char*>(&entry.confidence), sizeof(entry.confidence));
    output.write(reinterpret_cast<const char*>(&entry.active_partition_rows), sizeof(entry.active_partition_rows));
    output.write(reinterpret_cast<const char*>(&source_table_size), sizeof(source_table_size));
    output.write(entry.source_table.data(), static_cast<std::streamsize>(entry.source_table.size()));
    output.write(reinterpret_cast<const char*>(&source_column_size), sizeof(source_column_size));
    output.write(entry.source_column.data(), static_cast<std::streamsize>(entry.source_column.size()));
    output.write(reinterpret_cast<const char*>(&precision), sizeof(precision));
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

    std::uint8_t precision = 0;
    std::uint64_t register_count = 0;
    std::uint64_t partition_count = 0;

    input.read(reinterpret_cast<char*>(&entry.epsilon), sizeof(entry.epsilon));
    input.read(reinterpret_cast<char*>(&entry.confidence), sizeof(entry.confidence));
    input.read(reinterpret_cast<char*>(&entry.active_partition_rows), sizeof(entry.active_partition_rows));
    if (!read_string(input, entry.source_table) || !read_string(input, entry.source_column)) {
        return false;
    }
    input.read(reinterpret_cast<char*>(&precision), sizeof(precision));
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
        entry.partitions.emplace_back(precision, std::move(registers));
    }

    return true;
}

void invalidate_persisted_sketches(const skdb* db) {
    if (persistence_enabled(db)) {
        std::filesystem::remove_all(sketch_root(db));
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
                entry.partitions.emplace_back(entry.epsilon, entry.confidence);
                entry.active_partition_rows = 0;
            }
            entry.partitions.back().add(row[column_index]);
            ++entry.active_partition_rows;
        }
        handled_any = true;
    }

    return handled_any;
}

bool prewarm_streaming_hll_cache(skdb* db, const sketchydb::Plan& plan) {
    if (plan.mutation_kind != sketchydb::MutationKind::InsertValues || plan.insert_columns.empty()) {
        return false;
    }

    bool prewarmed_any = false;
    for (std::size_t column_index = 0; column_index < plan.insert_columns.size(); ++column_index) {
        const auto& column = plan.insert_columns[column_index];
        if (column.empty()) {
            continue;
        }

        const auto input_sql = hll_input_sql(plan.mutation_table, column);
        const auto cache_key = hll_cache_key(
            "approx_count_distinct",
            input_sql,
            kDefaultStreamingEpsilon,
            kDefaultStreamingConfidence);

        auto cached = db->hll_cache.find(cache_key);
        if (cached != db->hll_cache.end()) {
            continue;
        }

        auto inserted = db->hll_cache.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(cache_key),
            std::forward_as_tuple(
                kDefaultStreamingEpsilon,
                kDefaultStreamingConfidence,
                plan.mutation_table,
                column));
        cached = inserted.first;

        for (const auto& row : plan.insert_rows) {
            if (column_index >= row.size() || row[column_index].empty()) {
                return false;
            }
            if (cached->second.active_partition_rows >= kPartitionTargetRows) {
                cached->second.partitions.emplace_back(
                    cached->second.epsilon,
                    cached->second.confidence);
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
        if (plan.approximate_function != "approx_count_distinct") {
            return set_error(
                db,
                error_message,
                plan.approximate_function + " is recognized but not implemented yet");
        }

        const auto cache_key = hll_cache_key(plan);
        auto cached = db->hll_cache.find(cache_key);
        if (cached == db->hll_cache.end()) {
            auto inserted = db->hll_cache.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(cache_key),
                std::forward_as_tuple(
                    plan.epsilon,
                    plan.confidence,
                    plan.source_table,
                    plan.source_column));
            cached = inserted.first;

            if (!load_hll_entry(db, cache_key, cached->second)) {
                HllCallbackState state{.entry = &cached->second};
                std::string error;
                int rc = db->exact_backend->exec(
                    plan.input_sql.c_str(),
                    collect_hll_value,
                    &state,
                    error);
                if (rc != SKDB_OK) {
                    db->hll_cache.erase(cached);
                    return set_error(db, error_message, error);
                }
                save_hll_entry(db, cache_key, cached->second);
            }
        }

        int rc = emit_single_value(
            callback,
            user_data,
            std::to_string(estimate_partitioned_hll(cached->second)),
            "approx_count_distinct");
        if (rc != SKDB_OK) {
            return set_error(db, error_message, "query aborted by callback");
        }

        db->last_error.clear();
        return SKDB_OK;
    }

    std::string error;
    int rc = db->exact_backend->exec(sql, callback, user_data, error);
    if (rc != SKDB_OK) {
        return set_error(db, error_message, error);
    }
    if (plan.mutation_kind == sketchydb::MutationKind::InsertValues) {
        const bool updated_existing = add_inserted_values_to_hll_cache(db, plan);
        const bool prewarmed = prewarm_streaming_hll_cache(db, plan);
        for (const auto& [cache_key, entry] : db->hll_cache) {
            save_hll_entry(db, cache_key, entry);
        }
        if (updated_existing || prewarmed) {
            db->last_error.clear();
            return SKDB_OK;
        }
    }
    if (plan.invalidates_sketches) {
        db->hll_cache.clear();
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

void skdb_free(void* ptr) {
    delete[] static_cast<char*>(ptr);
}

const char* skdb_libversion(void) {
    return kVersion;
}
