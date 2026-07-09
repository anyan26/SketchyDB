#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sketchydb {

enum class ExecutionMode {
    Exact,
    Approximate,
};

enum class MutationKind {
    None,
    InsertValues,
    Other,
};

struct Plan {
    ExecutionMode mode = ExecutionMode::Exact;
    std::string approximate_function;
    std::string input_sql;
    std::string source_table;
    std::string source_column;
    std::string target_value;
    std::string sql_to_execute;
    std::uint64_t top_k = 0;
    double epsilon = 0.0;
    double confidence = 1.0;
    bool invalidates_sketches = false;
    bool has_approx_hint = false;
    double approx_hint_epsilon = 0.0;
    double approx_hint_confidence = 1.0;
    std::vector<std::string> approx_hint_columns;
    MutationKind mutation_kind = MutationKind::None;
    std::string mutation_table;
    std::vector<std::string> insert_columns;
    std::vector<std::vector<std::string>> insert_rows;
    std::string error_message;
};

class Planner {
public:
    [[nodiscard]] Plan plan(std::string_view sql) const;
};

}  // namespace sketchydb
