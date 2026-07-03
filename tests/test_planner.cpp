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
}

void recognizes_function_style_approximate_query() {
    assert_approx_count_distinct("select approx_count_distinct(user_id) from events");
}

void recognizes_function_case_insensitively() {
    assert_approx_count_distinct("select APPROX_COUNT_DISTINCT(user_id) from events");
    assert_approx_count_distinct("select ApPrOx_CoUnT_DiStInCt(user_id) from events");
}

void allows_whitespace_before_function_arguments() {
    assert_approx_count_distinct("select approx_count_distinct   (user_id) from events");
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
    assert_exact("select 'approx_count_distinct(user_id)' as label");
    assert_exact("select 'it''s approx_count_distinct(user_id)' as label");
    assert_exact("select \"approx_count_distinct(user_id)\" as identifier");
}

void comments_do_not_trigger_approximate_mode() {
    assert_exact("-- approx_count_distinct(user_id)\nselect count(*) from events");
    assert_exact("/* approx_count_distinct(user_id) */ select count(*) from events");
}

}  // namespace

int main() {
    recognizes_function_style_approximate_query();
    recognizes_function_case_insensitively();
    allows_whitespace_before_function_arguments();
    exact_query_stays_exact();
    identifier_without_call_stays_exact();
    string_literals_do_not_trigger_approximate_mode();
    comments_do_not_trigger_approximate_mode();
}
