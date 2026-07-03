#include "planner.hpp"

#include <cctype>

namespace sketchydb {

namespace {

bool is_identifier_char(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_';
}

bool equals_ignore_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index) {
        auto lhs = static_cast<unsigned char>(left[index]);
        auto rhs = static_cast<unsigned char>(right[index]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }

    return true;
}

std::size_t skip_sql_string(std::string_view sql, std::size_t index, char quote) {
    ++index;
    while (index < sql.size()) {
        if (sql[index] == quote) {
            if (index + 1 < sql.size() && sql[index + 1] == quote) {
                index += 2;
                continue;
            }
            return index + 1;
        }
        ++index;
    }
    return index;
}

std::size_t skip_line_comment(std::string_view sql, std::size_t index) {
    index += 2;
    while (index < sql.size() && sql[index] != '\n') {
        ++index;
    }
    return index;
}

std::size_t skip_block_comment(std::string_view sql, std::size_t index) {
    index += 2;
    while (index + 1 < sql.size()) {
        if (sql[index] == '*' && sql[index + 1] == '/') {
            return index + 2;
        }
        ++index;
    }
    return sql.size();
}

bool is_function_call(std::string_view sql, std::size_t index) {
    while (index < sql.size() && std::isspace(static_cast<unsigned char>(sql[index])) != 0) {
        ++index;
    }
    return index < sql.size() && sql[index] == '(';
}

bool contains_approx_count_distinct(std::string_view sql) {
    constexpr std::string_view kFunctionName = "approx_count_distinct";

    for (std::size_t index = 0; index < sql.size();) {
        if (sql[index] == '\'' || sql[index] == '"') {
            index = skip_sql_string(sql, index, sql[index]);
            continue;
        }
        if (index + 1 < sql.size() && sql[index] == '-' && sql[index + 1] == '-') {
            index = skip_line_comment(sql, index);
            continue;
        }
        if (index + 1 < sql.size() && sql[index] == '/' && sql[index + 1] == '*') {
            index = skip_block_comment(sql, index);
            continue;
        }
        if (!is_identifier_char(sql[index])) {
            ++index;
            continue;
        }

        const auto start = index;
        while (index < sql.size() && is_identifier_char(sql[index])) {
            ++index;
        }

        if (equals_ignore_case(sql.substr(start, index - start), kFunctionName) &&
            is_function_call(sql, index)) {
            return true;
        }
    }

    return false;
}

}  // namespace

Plan Planner::plan(std::string_view sql) const {
    // The first real decision point belongs here:
    //
    // SQL query
    //   -> SketchyDB planner
    //   -> decide exact vs approximate
    //   -> use sketches / sampling / cached metadata
    //   -> fall back to DuckDB when needed
    //
    if (contains_approx_count_distinct(sql)) {
        return Plan{
            .mode = ExecutionMode::Approximate,
            .approximate_function = "approx_count_distinct",
        };
    }

    return Plan{.mode = ExecutionMode::Exact};
}

}  // namespace sketchydb
