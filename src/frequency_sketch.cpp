#include "frequency_sketch.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace sketchydb {

namespace {

constexpr std::uint32_t kMinWidth = 16;
constexpr std::uint32_t kMinDepth = 2;
constexpr std::uint32_t kMaxWidth = 1U << 24U;
constexpr std::uint32_t kMaxDepth = 64;

std::uint64_t splitmix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::uint64_t fnv1a64(std::string_view value, std::uint64_t seed) {
    std::uint64_t hash = 14695981039346656037ULL ^ splitmix64(seed);
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return splitmix64(hash);
}

}  // namespace

std::uint32_t CountMinSketch::required_width(double epsilon) {
    if (epsilon <= 0.0 || epsilon > 1.0) {
        throw std::invalid_argument("invalid Count-Min epsilon");
    }
    const auto width = static_cast<std::uint64_t>(std::ceil(std::exp(1.0) / epsilon));
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(width, kMinWidth, kMaxWidth));
}

std::uint32_t CountMinSketch::required_depth(double confidence) {
    if (confidence <= 0.0 || confidence > 1.0) {
        throw std::invalid_argument("invalid Count-Min confidence");
    }
    const double delta = std::max(1.0 - confidence, 1e-12);
    const auto depth = static_cast<std::uint64_t>(std::ceil(std::log(1.0 / delta)));
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(depth, kMinDepth, kMaxDepth));
}

CountMinSketch::CountMinSketch(double epsilon, double confidence, std::uint64_t seed)
    : CountMinSketch(required_width(epsilon), required_depth(confidence), seed) {}

CountMinSketch::CountMinSketch(std::uint32_t width, std::uint32_t depth, std::uint64_t seed)
    : width_(std::clamp(width, kMinWidth, kMaxWidth)),
      depth_(std::clamp(depth, kMinDepth, kMaxDepth)),
      seed_(seed),
      counters_(static_cast<std::size_t>(width_) * depth_, 0) {}

CountMinSketch::CountMinSketch(
    std::uint32_t width,
    std::uint32_t depth,
    std::uint64_t seed,
    std::vector<std::uint64_t> counters)
    : width_(std::clamp(width, kMinWidth, kMaxWidth)),
      depth_(std::clamp(depth, kMinDepth, kMaxDepth)),
      seed_(seed),
      counters_(std::move(counters)) {
    if (counters_.size() != static_cast<std::size_t>(width_) * depth_) {
        counters_.assign(static_cast<std::size_t>(width_) * depth_, 0);
    }
}

void CountMinSketch::add(std::string_view value, std::uint64_t count) {
    for (std::uint32_t row = 0; row < depth_; ++row) {
        ++counters_[static_cast<std::size_t>(row) * width_ + hash(value, row)];
        if (count > 1) {
            counters_[static_cast<std::size_t>(row) * width_ + hash(value, row)] += count - 1;
        }
    }
}

std::uint64_t CountMinSketch::estimate(std::string_view value) const {
    std::uint64_t result = std::numeric_limits<std::uint64_t>::max();
    for (std::uint32_t row = 0; row < depth_; ++row) {
        result = std::min(result, counters_[static_cast<std::size_t>(row) * width_ + hash(value, row)]);
    }
    return result == std::numeric_limits<std::uint64_t>::max() ? 0 : result;
}

std::uint32_t CountMinSketch::width() const noexcept {
    return width_;
}

std::uint32_t CountMinSketch::depth() const noexcept {
    return depth_;
}

std::uint64_t CountMinSketch::seed() const noexcept {
    return seed_;
}

std::uint64_t CountMinSketch::memory_bytes() const noexcept {
    return static_cast<std::uint64_t>(sizeof(CountMinSketch)) +
           static_cast<std::uint64_t>(counters_.capacity() * sizeof(std::uint64_t));
}

const std::vector<std::uint64_t>& CountMinSketch::counters() const noexcept {
    return counters_;
}

std::uint64_t CountMinSketch::hash(std::string_view value, std::uint32_t row) const {
    return fnv1a64(value, seed_ ^ splitmix64(row)) % width_;
}

SpaceSavingTopK::SpaceSavingTopK(std::uint32_t capacity)
    : capacity_(std::max<std::uint32_t>(capacity, 1)) {}

SpaceSavingTopK::SpaceSavingTopK(std::uint32_t capacity, std::vector<TopKItem> items)
    : capacity_(std::max<std::uint32_t>(capacity, 1)) {
    for (const auto& item : items) {
        if (counters_.size() >= capacity_) {
            break;
        }
        counters_.emplace(item.value, Counter{.estimate = item.estimate, .error = item.error});
    }
}

void SpaceSavingTopK::add(std::string_view value) {
    auto found = counters_.find(std::string(value));
    if (found != counters_.end()) {
        ++found->second.estimate;
        return;
    }

    if (counters_.size() < capacity_) {
        counters_.emplace(std::string(value), Counter{.estimate = 1, .error = 0});
        return;
    }

    auto min_it = std::min_element(counters_.begin(), counters_.end(), [](const auto& left, const auto& right) {
        return left.second.estimate < right.second.estimate;
    });
    const auto replacement_count = min_it->second.estimate + 1;
    const auto replacement_error = min_it->second.estimate;
    counters_.erase(min_it);
    counters_.emplace(std::string(value), Counter{.estimate = replacement_count, .error = replacement_error});
}

std::vector<TopKItem> SpaceSavingTopK::top_k(std::uint32_t k) const {
    std::vector<TopKItem> items;
    items.reserve(counters_.size());
    for (const auto& [value, counter] : counters_) {
        items.push_back(TopKItem{
            .value = value,
            .estimate = counter.estimate,
            .error = counter.error,
        });
    }
    std::sort(items.begin(), items.end(), [](const auto& left, const auto& right) {
        if (left.estimate != right.estimate) {
            return left.estimate > right.estimate;
        }
        return left.value < right.value;
    });
    if (items.size() > k) {
        items.resize(k);
    }
    return items;
}

std::uint32_t SpaceSavingTopK::capacity() const noexcept {
    return capacity_;
}

std::uint64_t SpaceSavingTopK::memory_bytes() const noexcept {
    auto bytes = static_cast<std::uint64_t>(sizeof(SpaceSavingTopK));
    bytes += static_cast<std::uint64_t>(counters_.bucket_count() * sizeof(void*));
    for (const auto& [value, counter] : counters_) {
        (void)counter;
        bytes += static_cast<std::uint64_t>(sizeof(void*) * 2 + sizeof(std::string) + value.capacity() + sizeof(Counter));
    }
    return bytes;
}

}  // namespace sketchydb
