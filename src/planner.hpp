#pragma once

#include <string>
#include <string_view>

namespace sketchydb {

enum class ExecutionMode {
    Exact,
    Approximate,
};

struct Plan {
    ExecutionMode mode = ExecutionMode::Exact;
    std::string approximate_function;
};

class Planner {
public:
    [[nodiscard]] Plan plan(std::string_view sql) const;
};

}  // namespace sketchydb
