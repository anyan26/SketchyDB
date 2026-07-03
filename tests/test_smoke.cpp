#include "sketchydb.h"

#include <cassert>
#include <cstring>

int main() {
    skdb* db = nullptr;
    assert(skdb_open(":memory:", &db) == SKDB_OK);
    assert(db != nullptr);
    assert(std::strcmp(skdb_errmsg(db), "not an error") == 0);

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
    error_message = nullptr;
    rc = skdb_exec(
        db,
        "select APPROX_COUNT_DISTINCT(user_id, 0.01, 0.99) from events",
        nullptr,
        nullptr,
        &error_message);
    assert(rc == SKDB_ERROR);
    assert(error_message != nullptr);
    assert(std::strcmp(
               error_message,
               "approx_count_distinct is recognized but not implemented yet") == 0);
    skdb_free(error_message);

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
}
