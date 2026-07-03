#include "duckdb_backend.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#ifdef SKDB_USE_DUCKDB
#include <duckdb.h>
#include <vector>
#endif

namespace sketchydb {

struct DuckDBBackend::Impl {
#ifdef SKDB_USE_DUCKDB
    duckdb_database database = nullptr;
    duckdb_connection connection = nullptr;
#else
    std::string filename;
#endif
};

DuckDBBackend::DuckDBBackend(std::string filename) : impl_(std::make_unique<Impl>()) {
#ifdef SKDB_USE_DUCKDB
    if (duckdb_open(filename.c_str(), &impl_->database) == DuckDBError) {
        throw std::runtime_error("failed to open DuckDB database");
    }
    if (duckdb_connect(impl_->database, &impl_->connection) == DuckDBError) {
        duckdb_close(&impl_->database);
        throw std::runtime_error("failed to connect to DuckDB database");
    }
#else
    impl_->filename = std::move(filename);
#endif
}

DuckDBBackend::~DuckDBBackend() {
#ifdef SKDB_USE_DUCKDB
    if (impl_->connection != nullptr) {
        duckdb_disconnect(&impl_->connection);
    }
    if (impl_->database != nullptr) {
        duckdb_close(&impl_->database);
    }
#endif
}

int DuckDBBackend::exec(
    const char* sql,
    skdb_callback callback,
    void* user_data,
    std::string& error) {
#ifdef SKDB_USE_DUCKDB
    duckdb_result result;
    if (duckdb_query(impl_->connection, sql, &result) == DuckDBError) {
        const char* duckdb_error = duckdb_result_error(&result);
        error = duckdb_error == nullptr ? "DuckDB query failed" : duckdb_error;
        duckdb_destroy_result(&result);
        return SKDB_ERROR;
    }

    if (callback != nullptr) {
        const auto column_count = static_cast<int>(duckdb_column_count(&result));
        const auto row_count = duckdb_row_count(&result);
        std::vector<char*> names(static_cast<std::size_t>(column_count));
        std::vector<std::string> values_storage(static_cast<std::size_t>(column_count));
        std::vector<char*> values(static_cast<std::size_t>(column_count));

        for (int column = 0; column < column_count; ++column) {
            names[static_cast<std::size_t>(column)] =
                const_cast<char*>(duckdb_column_name(&result, static_cast<idx_t>(column)));
        }

        for (idx_t row = 0; row < row_count; ++row) {
            for (int column = 0; column < column_count; ++column) {
                char* raw_value = duckdb_value_varchar(&result, static_cast<idx_t>(column), row);
                values_storage[static_cast<std::size_t>(column)] =
                    raw_value == nullptr ? "" : raw_value;
                duckdb_free(raw_value);
                values[static_cast<std::size_t>(column)] =
                    values_storage[static_cast<std::size_t>(column)].data();
            }

            if (callback(user_data, column_count, values.data(), names.data()) != 0) {
                duckdb_destroy_result(&result);
                error = "query aborted by callback";
                return SKDB_ERROR;
            }
        }
    }

    duckdb_destroy_result(&result);
    return SKDB_OK;
#else
    (void)sql;
    (void)callback;
    (void)user_data;
    error = "DuckDB support is not compiled in; rebuild with SKDB_USE_DUCKDB=1";
    return SKDB_ERROR;
#endif
}

}  // namespace sketchydb
