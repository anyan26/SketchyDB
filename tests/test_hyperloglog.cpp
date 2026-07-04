#include "hyperloglog.hpp"

#include <cassert>
#include <cmath>
#include <stdexcept>
#include <string>

namespace {

void estimates_empty_stream_as_zero() {
    sketchydb::HyperLogLog hll(0.05, 0.90, 1);

    assert(hll.estimate() == 0.0);
}

void chooses_more_registers_for_stricter_bounds() {
    sketchydb::HyperLogLog loose(0.10, 0.80, 1);
    sketchydb::HyperLogLog strict(0.01, 0.99, 1);

    assert(strict.register_count() > loose.register_count());
    assert(sketchydb::HyperLogLog::required_precision(0.01, 0.99) >
           sketchydb::HyperLogLog::required_precision(0.10, 0.80));
}

void estimates_distinct_count_with_repeated_values() {
    sketchydb::HyperLogLog hll(0.05, 0.90, 1);

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
    sketchydb::HyperLogLog left(0.05, 0.90, 1);
    sketchydb::HyperLogLog right(0.05, 0.90, 1);

    for (int value = 0; value < 5000; ++value) {
        left.add("left-" + std::to_string(value));
        right.add("right-" + std::to_string(value));
    }

    left.merge(right);

    const double estimate = left.estimate();
    const double relative_error = std::abs(estimate - 10000.0) / 10000.0;
    assert(relative_error < 0.10);
}

void rejects_merge_with_different_hash_seed() {
    sketchydb::HyperLogLog left(0.05, 0.90, 1);
    sketchydb::HyperLogLog right(0.05, 0.90, 2);

    bool rejected = false;
    try {
        left.merge(right);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    assert(rejected);
}

}  // namespace

int main() {
    estimates_empty_stream_as_zero();
    chooses_more_registers_for_stricter_bounds();
    estimates_distinct_count_with_repeated_values();
    merges_partitioned_sketches();
    rejects_merge_with_different_hash_seed();
}
