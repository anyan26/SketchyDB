#include "planner.hpp"

#include <cassert>
#include <string>

namespace {

void assert_exact(std::string sql) {
    sketchydb::Planner planner;
    auto plan = planner.plan(sql);

    assert(plan.mode == sketchydb::ExecutionMode::Exact);
    assert(plan.approximate_function.empty());
}

void assert_approx_count_distinct(std::string sql) {
    sketchydb::Planner planner;
    auto plan = planner.plan(sql);

    assert(plan.mode == sketchydb::ExecutionMode::Approximate);
    assert(plan.approximate_function == "approx_count_distinct");
    assert(plan.error_message.empty());
}

void assert_invalid_approx(std::string sql, std::string expected_error) {
    sketchydb::Planner planner;
    auto plan = planner.plan(sql);

    assert(plan.mode == sketchydb::ExecutionMode::Approximate);
    assert(plan.approximate_function == "approx_count_distinct");
    assert(plan.error_message == expected_error);
}

void recognizes_function_style_approximate_query() {
    assert_approx_count_distinct("select approx_count_distinct(user_id, 0.01, 0.99) from events");
}

void recognizes_function_case_insensitively() {
    assert_approx_count_distinct("select APPROX_COUNT_DISTINCT(user_id, 0.01, 0.99) from events");
    assert_approx_count_distinct("select ApPrOx_CoUnT_DiStInCt(user_id, 0.01, 0.99) from events");
}

void allows_whitespace_before_function_arguments() {
    assert_approx_count_distinct("select approx_count_distinct   (user_id, 0.01, 0.99) from events");
}

void captures_epsilon_and_confidence() {
    sketchydb::Planner planner;
    auto plan = planner.plan("select approx_count_distinct(user_id, 0.05, 0.95) from events");

    assert(plan.mode == sketchydb::ExecutionMode::Approximate);
    assert(plan.approximate_function == "approx_count_distinct");
    assert(plan.epsilon == 0.05);
    assert(plan.confidence == 0.95);
}

void rejects_missing_epsilon_and_confidence() {
    assert_invalid_approx(
        "select approx_count_distinct(user_id) from events",
        "approx_count_distinct expects exactly 3 arguments: expression, epsilon, confidence");
}

void rejects_missing_confidence() {
    assert_invalid_approx(
        "select approx_count_distinct(user_id, 0.01) from events",
        "approx_count_distinct expects exactly 3 arguments: expression, epsilon, confidence");
}

void rejects_out_of_range_epsilon() {
    assert_invalid_approx(
        "select approx_count_distinct(user_id, 0.0, 0.99) from events",
        "approx_count_distinct epsilon must be a number in (0, 1]");
    assert_invalid_approx(
        "select approx_count_distinct(user_id, 1.1, 0.99) from events",
        "approx_count_distinct epsilon must be a number in (0, 1]");
}

void rejects_out_of_range_confidence() {
    assert_invalid_approx(
        "select approx_count_distinct(user_id, 0.01, 0.0) from events",
        "approx_count_distinct confidence must be a number in (0, 1]");
    assert_invalid_approx(
        "select approx_count_distinct(user_id, 0.01, 1.1) from events",
        "approx_count_distinct confidence must be a number in (0, 1]");
}

void rejects_non_numeric_bounds() {
    assert_invalid_approx(
        "select approx_count_distinct(user_id, epsilon, 0.99) from events",
        "approx_count_distinct epsilon must be a number in (0, 1]");
    assert_invalid_approx(
        "select approx_count_distinct(user_id, 0.01, confidence) from events",
        "approx_count_distinct confidence must be a number in (0, 1]");
}

void exact_query_stays_exact() {
    assert_exact("select count(distinct user_id) from events");
    assert_exact("select 1");
}

void identifier_without_call_stays_exact() {
    assert_exact("select approx_count_distinct from metrics");
    assert_exact("select approx_count_distinct_value from metrics");
}

void string_literals_do_not_trigger_approximate_mode() {
    assert_exact("select 'approx_count_distinct(user_id, 0.01, 0.99)' as label");
    assert_exact("select 'it''s approx_count_distinct(user_id, 0.01, 0.99)' as label");
    assert_exact("select \"approx_count_distinct(user_id, 0.01, 0.99)\" as identifier");
}

void comments_do_not_trigger_approximate_mode() {
    assert_exact("-- approx_count_distinct(user_id, 0.01, 0.99)\nselect count(*) from events");
    assert_exact("/* approx_count_distinct(user_id, 0.01, 0.99) */ select count(*) from events");
}

void table_and_column_names_with_approx_prefix_stay_exact() {
    assert_exact("select user_id from approx_events");
    assert_exact("select approx_count_distinct from events");
    assert_exact("select approx_table.approx_count_distinct from approx_table");
    assert_exact("select approx_count_distinct_value from approx_metrics");
}

}  // namespace

int main() {
    recognizes_function_style_approximate_query();
    recognizes_function_case_insensitively();
    allows_whitespace_before_function_arguments();
    captures_epsilon_and_confidence();
    rejects_missing_epsilon_and_confidence();
    rejects_missing_confidence();
    rejects_out_of_range_epsilon();
    rejects_out_of_range_confidence();
    rejects_non_numeric_bounds();
    exact_query_stays_exact();
    identifier_without_call_stays_exact();
    string_literals_do_not_trigger_approximate_mode();
    comments_do_not_trigger_approximate_mode();
    table_and_column_names_with_approx_prefix_stay_exact();
}
