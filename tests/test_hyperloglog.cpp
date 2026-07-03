#include "hyperloglog.hpp"

#include <cassert>
#include <cmath>
#include <string>

namespace {

void estimates_empty_stream_as_zero() {
    sketchydb::HyperLogLog hll(0.05, 0.90);

    assert(hll.estimate() == 0.0);
}

void chooses_more_registers_for_stricter_bounds() {
    sketchydb::HyperLogLog loose(0.10, 0.80);
    sketchydb::HyperLogLog strict(0.01, 0.99);

    assert(strict.register_count() > loose.register_count());
}

void estimates_distinct_count_with_repeated_values() {
    sketchydb::HyperLogLog hll(0.05, 0.90);

    for (int repeat = 0; repeat < 5; ++repeat) {
        for (int value = 0; value < 10000; ++value) {
            hll.add("user-" + std::to_string(value));
        }
    }

    const double estimate = hll.estimate();
    const double relative_error = std::abs(estimate - 10000.0) / 10000.0;
    assert(relative_error < 0.10);
}

void merges_partitioned_sketches() {
    sketchydb::HyperLogLog left(0.05, 0.90);
    sketchydb::HyperLogLog right(0.05, 0.90);

    for (int value = 0; value < 5000; ++value) {
        left.add("left-" + std::to_string(value));
        right.add("right-" + std::to_string(value));
    }

    left.merge(right);

    const double estimate = left.estimate();
    const double relative_error = std::abs(estimate - 10000.0) / 10000.0;
    assert(relative_error < 0.10);
}

}  // namespace

int main() {
    estimates_empty_stream_as_zero();
    chooses_more_registers_for_stricter_bounds();
    estimates_distinct_count_with_repeated_values();
    merges_partitioned_sketches();
}
