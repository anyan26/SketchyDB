#pragma once

#include <string_view>

namespace sketchydb {

enum class ExecutionMode {
    Exact,
    Approximate,
};

struct Plan {
    ExecutionMode mode = ExecutionMode::Exact;
};

class Planner {
public:
    [[nodiscard]] Plan plan(std::string_view sql) const;
};

}  // namespace sketchydb
