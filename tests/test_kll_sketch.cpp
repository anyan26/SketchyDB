#include "kll_sketch.hpp"

#include <cassert>
#include <cmath>

namespace {

void empty_sketch_returns_zero() {
    sketchydb::KllSketch sketch(0.05, 0.90, 1);

    assert(sketch.count() == 0);
    assert(sketch.quantile(0.50) == 0.0);
}

void stricter_bounds_use_more_capacity() {
    const auto loose = sketchydb::KllSketch::required_capacity(0.10, 0.80);
    const auto strict = sketchydb::KllSketch::required_capacity(0.01, 0.99);

    assert(strict > loose);
}

void estimates_median_and_quartiles() {
    sketchydb::KllSketch sketch(0.02, 0.99, 7);
    for (int value = 0; value < 10000; ++value) {
        sketch.add(static_cast<double>(value));
    }

    assert(std::abs(sketch.quantile(0.25) - 2500.0) < 500.0);
    assert(std::abs(sketch.quantile(0.50) - 5000.0) < 500.0);
    assert(std::abs(sketch.quantile(0.75) - 7500.0) < 500.0);
    assert(sketch.memory_bytes() > 0);
}

void ignores_non_finite_values() {
    sketchydb::KllSketch sketch(0.05, 0.90, 1);

    sketch.add(1.0);
    sketch.add(NAN);
    sketch.add(INFINITY);

    assert(sketch.count() == 1);
    assert(sketch.quantile(0.50) == 1.0);
}

void same_seed_reproduces_random_compaction_choices() {
    sketchydb::KllSketch left(32, 1234);
    sketchydb::KllSketch right(32, 1234);

    for (int value = 0; value < 5000; ++value) {
        left.add(static_cast<double>(value));
        right.add(static_cast<double>(value));
    }

    assert(left.quantile(0.25) == right.quantile(0.25));
    assert(left.quantile(0.50) == right.quantile(0.50));
    assert(left.quantile(0.75) == right.quantile(0.75));
}

}  // namespace

int main() {
    empty_sketch_returns_zero();
    stricter_bounds_use_more_capacity();
    estimates_median_and_quartiles();
    ignores_non_finite_values();
    same_seed_reproduces_random_compaction_choices();
}
