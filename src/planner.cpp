#include "planner.hpp"

namespace sketchydb {

Plan Planner::plan(std::string_view sql) const {
    (void)sql;

    // The first real decision point belongs here:
    //
    // SQL query
    //   -> SketchyDB planner
    //   -> decide exact vs approximate
    //   -> use sketches / sampling / cached metadata
    //   -> fall back to DuckDB when needed
    //
    // For now, every query is exact so the project can grow around DuckDB
    // without prematurely implementing the randomized algorithm layer.
    return Plan{.mode = ExecutionMode::Exact};
}

}  // namespace sketchydb
