#include "sketchydb.h"

#include "duckdb_backend.hpp"
#include "planner.hpp"

#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>

struct skdb {
    std::string filename;
    std::string last_error;
    sketchydb::Planner planner;
    std::unique_ptr<sketchydb::DuckDBBackend> exact_backend;
};

namespace {

constexpr const char* kVersion = "0.1.0";

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
        return set_error(
            db,
            error_message,
            plan.approximate_function + " is recognized but not implemented yet");
    }

    std::string error;
    int rc = db->exact_backend->exec(sql, callback, user_data, error);
    if (rc != SKDB_OK) {
        return set_error(db, error_message, error);
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
