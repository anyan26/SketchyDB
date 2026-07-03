#include "planner.hpp"

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace sketchydb {

namespace {

enum class TokenKind {
    Identifier,
    Number,
    LeftParen,
    RightParen,
    Dot,
    Comma,
    Other,
};

struct Token {
    TokenKind kind = TokenKind::Other;
    std::string text;
};

struct ApproxCall {
    std::string function_name;
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

std::string lowercase(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (char character : value) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
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

std::vector<Token> tokenize_sql(std::string_view sql) {
    std::vector<Token> tokens;

    for (std::size_t index = 0; index < sql.size();) {
        if (std::isspace(static_cast<unsigned char>(sql[index])) != 0) {
            ++index;
            continue;
        }
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
        if (is_identifier_start(sql[index])) {
            const auto start = index;
            while (index < sql.size() && is_identifier_char(sql[index])) {
                ++index;
            }
            tokens.push_back(Token{
                .kind = TokenKind::Identifier,
                .text = std::string(sql.substr(start, index - start)),
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
                });
                continue;
            }
            index = start;
        }

        switch (sql[index]) {
            case '(':
                tokens.push_back(Token{.kind = TokenKind::LeftParen, .text = "("});
                break;
            case ')':
                tokens.push_back(Token{.kind = TokenKind::RightParen, .text = ")"});
                break;
            case '.':
                tokens.push_back(Token{.kind = TokenKind::Dot, .text = "."});
                break;
            case ',':
                tokens.push_back(Token{.kind = TokenKind::Comma, .text = ","});
                break;
            default:
                tokens.push_back(Token{.kind = TokenKind::Other, .text = std::string(1, sql[index])});
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

ApproxCall parse_approx_arguments(
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

    if (top_level_commas.size() != 2) {
        return ApproxCall{
            .function_name = function_name,
            .error_message = function_name + " expects exactly 3 arguments: expression, epsilon, confidence",
        };
    }

    const auto epsilon_index = top_level_commas[0] + 1;
    const auto confidence_index = top_level_commas[1] + 1;
    if (epsilon_index >= top_level_commas[1] || tokens[epsilon_index].kind != TokenKind::Number) {
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

    if (epsilon_index + 1 != top_level_commas[1]) {
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
            return parse_approx_arguments(tokens, index, left_paren, right_paren);
        }
    }

    return ApproxCall{};
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
    auto approximate_call = parse_approx_function(sql);
    if (!approximate_call.function_name.empty()) {
        return Plan{
            .mode = ExecutionMode::Approximate,
            .approximate_function = std::move(approximate_call.function_name),
            .epsilon = approximate_call.epsilon,
            .confidence = approximate_call.confidence,
            .error_message = std::move(approximate_call.error_message),
        };
    }

    return Plan{.mode = ExecutionMode::Exact};
}

}  // namespace sketchydb
