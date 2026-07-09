#include "planner.hpp"

#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace sketchydb {

namespace {

enum class TokenKind {
    Identifier,
    Number,
    String,
    LeftParen,
    RightParen,
    Dot,
    Comma,
    Other,
};

struct Token {
    TokenKind kind = TokenKind::Other;
    std::string text;
    std::size_t start = 0;
    std::size_t end = 0;
};

struct ApproxCall {
    std::string function_name;
    std::string input_sql;
    std::string source_table;
    std::string source_column;
    std::string target_value;
    std::uint64_t top_k = 0;
    double epsilon = 0.0;
    double confidence = 1.0;
    std::string error_message;
};

bool is_identifier_char(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_';
}

bool is_identifier_start(char value) {
    return std::isalpha(static_cast<unsigned char>(value)) != 0 || value == '_';
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

bool starts_with_ignore_case(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && equals_ignore_case(value.substr(0, prefix.size()), prefix);
}

bool is_mutation_keyword(std::string_view value) {
    return equals_ignore_case(value, "insert") ||
           equals_ignore_case(value, "update") ||
           equals_ignore_case(value, "alter") ||
           equals_ignore_case(value, "delete") ||
           equals_ignore_case(value, "drop") ||
           equals_ignore_case(value, "create") ||
           equals_ignore_case(value, "truncate") ||
           equals_ignore_case(value, "copy") ||
           equals_ignore_case(value, "replace");
}

bool contains_mutation_statement(const std::vector<Token>& tokens) {
    bool at_statement_start = true;

    for (const auto& token : tokens) {
        if (token.kind == TokenKind::Other && token.text == ";") {
            at_statement_start = true;
            continue;
        }
        if (at_statement_start && token.kind == TokenKind::Identifier) {
            if (is_mutation_keyword(token.text)) {
                return true;
            }
            at_statement_start = false;
            continue;
        }
        if (token.kind != TokenKind::Other || token.text != ";") {
            at_statement_start = false;
        }
    }

    return false;
}

std::string lowercase(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (char character : value) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

Token read_sql_string(std::string_view sql, std::size_t index, char quote) {
    const auto start = index;
    std::string value;
    ++index;
    while (index < sql.size()) {
        if (sql[index] == quote) {
            if (index + 1 < sql.size() && sql[index + 1] == quote) {
                value.push_back(quote);
                index += 2;
                continue;
            }
            ++index;
            break;
        }
        value.push_back(sql[index]);
        ++index;
    }
    return Token{
        .kind = TokenKind::String,
        .text = value,
        .start = start,
        .end = index,
    };
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

std::vector<Token> tokenize_sql(std::string_view sql) {
    std::vector<Token> tokens;

    for (std::size_t index = 0; index < sql.size();) {
        if (std::isspace(static_cast<unsigned char>(sql[index])) != 0) {
            ++index;
            continue;
        }
        if (sql[index] == '\'' || sql[index] == '"') {
            auto token = read_sql_string(sql, index, sql[index]);
            index = token.end;
            tokens.push_back(std::move(token));
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
        if (is_identifier_start(sql[index])) {
            const auto start = index;
            while (index < sql.size() && is_identifier_char(sql[index])) {
                ++index;
            }
            tokens.push_back(Token{
                .kind = TokenKind::Identifier,
                .text = std::string(sql.substr(start, index - start)),
                .start = start,
                .end = index,
            });
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(sql[index])) != 0 || sql[index] == '.') {
            const auto start = index;
            bool saw_digit = false;
            bool saw_dot = false;
            while (index < sql.size()) {
                if (std::isdigit(static_cast<unsigned char>(sql[index])) != 0) {
                    saw_digit = true;
                    ++index;
                    continue;
                }
                if (sql[index] == '.' && !saw_dot) {
                    saw_dot = true;
                    ++index;
                    continue;
                }
                break;
            }
            if (saw_digit) {
                tokens.push_back(Token{
                    .kind = TokenKind::Number,
                    .text = std::string(sql.substr(start, index - start)),
                    .start = start,
                    .end = index,
                });
                continue;
            }
            index = start;
        }

        switch (sql[index]) {
            case '(':
                tokens.push_back(Token{.kind = TokenKind::LeftParen, .text = "(", .start = index, .end = index + 1});
                break;
            case ')':
                tokens.push_back(Token{.kind = TokenKind::RightParen, .text = ")", .start = index, .end = index + 1});
                break;
            case '.':
                tokens.push_back(Token{.kind = TokenKind::Dot, .text = ".", .start = index, .end = index + 1});
                break;
            case ',':
                tokens.push_back(Token{.kind = TokenKind::Comma, .text = ",", .start = index, .end = index + 1});
                break;
            default:
                tokens.push_back(Token{
                    .kind = TokenKind::Other,
                    .text = std::string(1, sql[index]),
                    .start = index,
                    .end = index + 1,
                });
                break;
        }
        ++index;
    }

    return tokens;
}

bool is_approx_function_call(const std::vector<Token>& tokens, std::size_t index) {
    if (tokens[index].kind != TokenKind::Identifier ||
        !starts_with_ignore_case(tokens[index].text, "approx_")) {
        return false;
    }

    const bool is_qualified_name =
        index > 0 && tokens[index - 1].kind == TokenKind::Dot;
    const bool is_called =
        index + 1 < tokens.size() && tokens[index + 1].kind == TokenKind::LeftParen;

    return !is_qualified_name && is_called;
}

std::size_t find_matching_right_paren(const std::vector<Token>& tokens, std::size_t left_paren) {
    std::size_t depth = 0;
    for (std::size_t index = left_paren; index < tokens.size(); ++index) {
        if (tokens[index].kind == TokenKind::LeftParen) {
            ++depth;
        } else if (tokens[index].kind == TokenKind::RightParen) {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return tokens.size();
}

bool parse_number(std::string_view value, double& out_number) {
    std::string owned(value);
    char* end = nullptr;
    out_number = std::strtod(owned.c_str(), &end);
    return end != owned.c_str() && *end == '\0';
}

bool parse_uint64(std::string_view value, std::uint64_t& out_number) {
    std::string owned(value);
    char* end = nullptr;
    out_number = std::strtoull(owned.c_str(), &end, 10);
    return end != owned.c_str() && *end == '\0';
}

std::string parse_simple_column(
    const std::vector<Token>& tokens,
    std::size_t expression_start,
    std::size_t expression_end) {
    std::vector<Token> expression_tokens;
    for (const auto& token : tokens) {
        if (token.start >= expression_start && token.end <= expression_end) {
            expression_tokens.push_back(token);
        }
    }

    if (expression_tokens.size() == 1 && expression_tokens[0].kind == TokenKind::Identifier) {
        return lowercase(expression_tokens[0].text);
    }
    if (expression_tokens.size() == 3 &&
        expression_tokens[0].kind == TokenKind::Identifier &&
        expression_tokens[1].kind == TokenKind::Dot &&
        expression_tokens[2].kind == TokenKind::Identifier) {
        return lowercase(expression_tokens[2].text);
    }

    return "";
}

std::string parse_source_table_after(const std::vector<Token>& tokens, std::size_t start_index) {
    for (std::size_t index = start_index; index + 1 < tokens.size(); ++index) {
        if (tokens[index].kind == TokenKind::Identifier &&
            equals_ignore_case(tokens[index].text, "from") &&
            tokens[index + 1].kind == TokenKind::Identifier) {
            return lowercase(tokens[index + 1].text);
        }
    }
    return "";
}

bool is_single_value_token(
    const std::vector<Token>& tokens,
    std::size_t argument_start,
    std::size_t argument_end,
    std::string& value) {
    std::vector<Token> argument_tokens;
    for (const auto& token : tokens) {
        if (token.start >= argument_start && token.end <= argument_end) {
            argument_tokens.push_back(token);
        }
    }
    if (argument_tokens.size() != 1) {
        return false;
    }
    if (argument_tokens[0].kind != TokenKind::String &&
        argument_tokens[0].kind != TokenKind::Number &&
        argument_tokens[0].kind != TokenKind::Identifier) {
        return false;
    }
    value = argument_tokens[0].text;
    return true;
}

ApproxCall parse_approx_arguments(
    std::string_view sql,
    const std::vector<Token>& tokens,
    std::size_t function_index,
    std::size_t left_paren,
    std::size_t right_paren) {
    const auto function_name = lowercase(tokens[function_index].text);
    std::size_t depth = 0;
    std::vector<std::size_t> top_level_commas;

    for (std::size_t index = left_paren + 1; index < right_paren; ++index) {
        if (tokens[index].kind == TokenKind::LeftParen) {
            ++depth;
        } else if (tokens[index].kind == TokenKind::RightParen) {
            if (depth > 0) {
                --depth;
            }
        } else if (tokens[index].kind == TokenKind::Comma && depth == 0) {
            top_level_commas.push_back(index);
        }
    }

    const bool is_freq = function_name == "approx_freq";
    const bool is_top_k = function_name == "approx_top_k";
    const std::size_t expected_commas = is_freq || is_top_k ? 3 : 2;
    if (top_level_commas.size() != expected_commas) {
        return ApproxCall{
            .function_name = function_name,
            .error_message = function_name + (expected_commas == 3
                ? " expects exactly 4 arguments"
                : " expects exactly 3 arguments: expression, epsilon, confidence"),
        };
    }

    const auto expression_start = tokens[left_paren].end;
    const auto expression_end = tokens[top_level_commas[0]].start;
    if (expression_start >= expression_end) {
        return ApproxCall{
            .function_name = function_name,
            .error_message = function_name + " expression argument cannot be empty",
        };
    }

    std::string target_value;
    std::uint64_t top_k = 0;
    std::size_t epsilon_comma_index = 0;
    std::size_t confidence_comma_index = 1;
    if (is_freq) {
        const auto target_start = tokens[top_level_commas[0]].end;
        const auto target_end = tokens[top_level_commas[1]].start;
        if (!is_single_value_token(tokens, target_start, target_end, target_value)) {
            return ApproxCall{
                .function_name = function_name,
                .error_message = function_name + " target value must be a single string, number, or identifier literal",
            };
        }
        epsilon_comma_index = 1;
        confidence_comma_index = 2;
    } else if (is_top_k) {
        const auto k_index = top_level_commas[0] + 1;
        if (k_index >= top_level_commas[1] ||
            tokens[k_index].kind != TokenKind::Number ||
            k_index + 1 != top_level_commas[1] ||
            !parse_uint64(tokens[k_index].text, top_k) ||
            top_k == 0) {
            return ApproxCall{
                .function_name = function_name,
                .error_message = function_name + " k must be a positive integer literal",
            };
        }
        epsilon_comma_index = 1;
        confidence_comma_index = 2;
    }

    const auto epsilon_index = top_level_commas[epsilon_comma_index] + 1;
    const auto confidence_index = top_level_commas[confidence_comma_index] + 1;
    if (epsilon_index >= top_level_commas[confidence_comma_index] || tokens[epsilon_index].kind != TokenKind::Number) {
        return ApproxCall{
            .function_name = function_name,
            .error_message = function_name + " epsilon must be a number in (0, 1]",
        };
    }

    double epsilon = 0.0;
    if (!parse_number(tokens[epsilon_index].text, epsilon) || epsilon <= 0.0 || epsilon > 1.0) {
        return ApproxCall{
            .function_name = function_name,
            .error_message = function_name + " epsilon must be a number in (0, 1]",
        };
    }

    if (epsilon_index + 1 != top_level_commas[confidence_comma_index]) {
        return ApproxCall{
            .function_name = function_name,
            .error_message = function_name + " epsilon must be a single numeric literal",
        };
    }

    if (confidence_index >= right_paren || tokens[confidence_index].kind != TokenKind::Number) {
        return ApproxCall{
            .function_name = function_name,
            .error_message = function_name + " confidence must be a number in (0, 1]",
        };
    }

    double confidence = 0.0;
    if (!parse_number(tokens[confidence_index].text, confidence) ||
        confidence <= 0.0 || confidence > 1.0) {
        return ApproxCall{
            .function_name = function_name,
            .error_message = function_name + " confidence must be a number in (0, 1]",
        };
    }

    if (confidence_index + 1 != right_paren) {
        return ApproxCall{
            .function_name = function_name,
            .error_message = function_name + " confidence must be a single numeric literal",
        };
    }

    return ApproxCall{
        .function_name = function_name,
        .input_sql = "select " + std::string(sql.substr(expression_start, expression_end - expression_start)) +
                     " as skdb_hll_value" + std::string(sql.substr(tokens[right_paren].end)),
        .source_table = parse_source_table_after(tokens, right_paren + 1),
        .source_column = parse_simple_column(tokens, expression_start, expression_end),
        .target_value = std::move(target_value),
        .top_k = top_k,
        .epsilon = epsilon,
        .confidence = confidence,
    };
}

ApproxCall parse_approx_function(std::string_view sql) {
    const auto tokens = tokenize_sql(sql);
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (is_approx_function_call(tokens, index)) {
            const auto left_paren = index + 1;
            const auto right_paren = find_matching_right_paren(tokens, left_paren);
            if (right_paren == tokens.size()) {
                return ApproxCall{
                    .function_name = lowercase(tokens[index].text),
                    .error_message = lowercase(tokens[index].text) + " is missing a closing ')'",
                };
            }
            return parse_approx_arguments(sql, tokens, index, left_paren, right_paren);
        }
    }

    return ApproxCall{};
}

std::size_t skip_parenthesized_group(const std::vector<Token>& tokens, std::size_t left_paren) {
    return find_matching_right_paren(tokens, left_paren);
}

std::vector<std::string> parse_identifier_list(
    const std::vector<Token>& tokens,
    std::size_t left_paren,
    std::size_t right_paren) {
    std::vector<std::string> columns;
    for (std::size_t index = left_paren + 1; index < right_paren; ++index) {
        if (tokens[index].kind == TokenKind::Identifier) {
            columns.push_back(lowercase(tokens[index].text));
        }
    }
    return columns;
}

std::vector<std::string> parse_insert_row(
    const std::vector<Token>& tokens,
    std::size_t left_paren,
    std::size_t right_paren) {
    std::vector<std::string> values;
    std::size_t depth = 0;
    std::size_t value_start = left_paren + 1;

    for (std::size_t index = left_paren + 1; index <= right_paren; ++index) {
        if (index < right_paren && tokens[index].kind == TokenKind::LeftParen) {
            ++depth;
            continue;
        }
        if (index < right_paren && tokens[index].kind == TokenKind::RightParen && depth > 0) {
            --depth;
            continue;
        }
        const bool at_separator =
            index == right_paren || (tokens[index].kind == TokenKind::Comma && depth == 0);
        if (!at_separator) {
            continue;
        }

        if (value_start < index && value_start + 1 == index &&
            (tokens[value_start].kind == TokenKind::String || tokens[value_start].kind == TokenKind::Number ||
             tokens[value_start].kind == TokenKind::Identifier)) {
            values.push_back(tokens[value_start].text);
        } else {
            values.push_back("");
        }
        value_start = index + 1;
    }

    return values;
}

bool parse_approx_insert_hint(const std::vector<Token>& tokens, std::size_t hint_index, Plan& plan, std::size_t& after_hint) {
    if (hint_index + 2 >= tokens.size() ||
        tokens[hint_index].kind != TokenKind::Identifier ||
        !equals_ignore_case(tokens[hint_index].text, "approx_hint") ||
        tokens[hint_index + 1].kind != TokenKind::LeftParen ||
        tokens[hint_index + 2].kind != TokenKind::LeftParen) {
        return false;
    }

    const auto columns_end = skip_parenthesized_group(tokens, hint_index + 2);
    if (columns_end == tokens.size() || columns_end + 5 >= tokens.size()) {
        return false;
    }
    if (tokens[columns_end + 1].kind != TokenKind::Comma ||
        tokens[columns_end + 2].kind != TokenKind::Number ||
        tokens[columns_end + 3].kind != TokenKind::Comma ||
        tokens[columns_end + 4].kind != TokenKind::Number ||
        tokens[columns_end + 5].kind != TokenKind::RightParen) {
        return false;
    }

    double epsilon = 0.0;
    double confidence = 0.0;
    if (!parse_number(tokens[columns_end + 2].text, epsilon) ||
        !parse_number(tokens[columns_end + 4].text, confidence) ||
        epsilon <= 0.0 || epsilon > 1.0 ||
        confidence <= 0.0 || confidence > 1.0) {
        return false;
    }

    plan.has_approx_hint = true;
    plan.approx_hint_columns = parse_identifier_list(tokens, hint_index + 2, columns_end);
    plan.approx_hint_epsilon = epsilon;
    plan.approx_hint_confidence = confidence;
    after_hint = columns_end + 6;
    return !plan.approx_hint_columns.empty();
}

Plan parse_insert_values_plan(std::string_view sql, const std::vector<Token>& tokens) {
    if (tokens.size() < 4 ||
        tokens[0].kind != TokenKind::Identifier ||
        !equals_ignore_case(tokens[0].text, "insert")) {
        return Plan{
            .mode = ExecutionMode::Exact,
            .invalidates_sketches = true,
            .mutation_kind = MutationKind::Other,
        };
    }

    Plan plan{
        .mode = ExecutionMode::Exact,
        .invalidates_sketches = true,
        .mutation_kind = MutationKind::Other,
    };

    std::size_t into_index = 1;
    if (into_index < tokens.size() &&
        tokens[into_index].kind == TokenKind::Identifier &&
        equals_ignore_case(tokens[into_index].text, "approx_hint")) {
        if (!parse_approx_insert_hint(tokens, into_index, plan, into_index)) {
            return plan;
        }
    }

    if (into_index + 1 >= tokens.size() ||
        tokens[into_index].kind != TokenKind::Identifier ||
        !equals_ignore_case(tokens[into_index].text, "into") ||
        tokens[into_index + 1].kind != TokenKind::Identifier) {
        return plan;
    }

    plan.mutation_kind = MutationKind::InsertValues;
    plan.mutation_table = lowercase(tokens[into_index + 1].text);

    if (plan.has_approx_hint) {
        plan.sql_to_execute = "insert " + std::string(sql.substr(tokens[into_index].start));
    }
    std::size_t index = into_index + 2;
    if (index < tokens.size() && tokens[index].kind == TokenKind::LeftParen) {
        const auto columns_end = skip_parenthesized_group(tokens, index);
        if (columns_end == tokens.size()) {
            plan.mutation_kind = MutationKind::Other;
            return plan;
        }
        plan.insert_columns = parse_identifier_list(tokens, index, columns_end);
        index = columns_end + 1;
    }

    if (index >= tokens.size() ||
        tokens[index].kind != TokenKind::Identifier ||
        !equals_ignore_case(tokens[index].text, "values")) {
        plan.mutation_kind = MutationKind::Other;
        return plan;
    }
    ++index;

    while (index < tokens.size()) {
        if (tokens[index].kind == TokenKind::Other && tokens[index].text == ";") {
            break;
        }
        if (tokens[index].kind == TokenKind::Comma) {
            ++index;
            continue;
        }
        if (tokens[index].kind != TokenKind::LeftParen) {
            plan.mutation_kind = MutationKind::Other;
            return plan;
        }
        const auto row_end = skip_parenthesized_group(tokens, index);
        if (row_end == tokens.size()) {
            plan.mutation_kind = MutationKind::Other;
            return plan;
        }
        plan.insert_rows.push_back(parse_insert_row(tokens, index, row_end));
        index = row_end + 1;
    }

    if (plan.insert_rows.empty()) {
        plan.mutation_kind = MutationKind::Other;
    }
    return plan;
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
    const auto tokens = tokenize_sql(sql);
    if (!tokens.empty() &&
        tokens[0].kind == TokenKind::Identifier &&
        equals_ignore_case(tokens[0].text, "insert")) {
        return parse_insert_values_plan(sql, tokens);
    }

    auto approximate_call = parse_approx_function(sql);
    if (!approximate_call.function_name.empty()) {
        return Plan{
            .mode = ExecutionMode::Approximate,
            .approximate_function = std::move(approximate_call.function_name),
            .input_sql = std::move(approximate_call.input_sql),
            .source_table = std::move(approximate_call.source_table),
            .source_column = std::move(approximate_call.source_column),
            .target_value = std::move(approximate_call.target_value),
            .top_k = approximate_call.top_k,
            .epsilon = approximate_call.epsilon,
            .confidence = approximate_call.confidence,
            .error_message = std::move(approximate_call.error_message),
        };
    }

    if (contains_mutation_statement(tokens)) {
        return Plan{
            .mode = ExecutionMode::Exact,
            .invalidates_sketches = true,
            .mutation_kind = MutationKind::Other,
        };
    }

    return Plan{.mode = ExecutionMode::Exact};
}

}  // namespace sketchydb
