#include "frequency_sketch.hpp"

#include <cassert>

namespace {

void count_min_never_underestimates_seen_values() {
    sketchydb::CountMinSketch sketch(0.01, 0.99, 42);

    for (int index = 0; index < 100; ++index) {
        sketch.add("a");
    }
    for (int index = 0; index < 30; ++index) {
        sketch.add("b");
    }

    assert(sketch.estimate("a") >= 100);
    assert(sketch.estimate("b") >= 30);
    assert(sketch.estimate("missing") <= sketch.estimate("b"));
    assert(sketch.memory_bytes() > 0);
}

void stricter_bounds_use_larger_count_min_tables() {
    assert(sketchydb::CountMinSketch::required_width(0.01) >
           sketchydb::CountMinSketch::required_width(0.10));
    assert(sketchydb::CountMinSketch::required_depth(0.99) >
           sketchydb::CountMinSketch::required_depth(0.80));
}

void top_k_tracks_heavy_values() {
    sketchydb::SpaceSavingTopK top_k(3);

    for (int index = 0; index < 100; ++index) {
        top_k.add("hot");
    }
    for (int index = 0; index < 50; ++index) {
        top_k.add("warm");
    }
    for (int index = 0; index < 10; ++index) {
        top_k.add("cold");
    }

    const auto items = top_k.top_k(2);
    assert(items.size() == 2);
    assert(items[0].value == "hot");
    assert(items[0].estimate >= items[1].estimate);
    assert(top_k.memory_bytes() > 0);
}

}  // namespace

int main() {
    count_min_never_underestimates_seen_values();
    stricter_bounds_use_larger_count_min_tables();
    top_k_tracks_heavy_values();
}
