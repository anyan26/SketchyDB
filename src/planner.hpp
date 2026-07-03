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
    double epsilon = 0.0;
    double confidence = 1.0;
    std::string error_message;
};

class Planner {
public:
    [[nodiscard]] Plan plan(std::string_view sql) const;
};

}  // namespace sketchydb
