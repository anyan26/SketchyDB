#pragma once

#include "sketchydb.h"

#include <memory>
#include <string>

namespace sketchydb {

class DuckDBBackend {
public:
    explicit DuckDBBackend(std::string filename);
    ~DuckDBBackend();

    DuckDBBackend(const DuckDBBackend&) = delete;
    DuckDBBackend& operator=(const DuckDBBackend&) = delete;

    [[nodiscard]] int exec(
        const char* sql,
        skdb_callback callback,
        void* user_data,
        std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sketchydb
