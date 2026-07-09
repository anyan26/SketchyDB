#include "kll_sketch.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace sketchydb {

namespace {

constexpr std::uint32_t kMinCapacity = 32;
constexpr std::uint32_t kMaxCapacity = 1U << 20U;

std::uint64_t splitmix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::size_t sample_compaction_offset(std::mt19937_64& random) {
    std::bernoulli_distribution choose_odd_position(0.5);
    return choose_odd_position(random) ? 1U : 0U;
}

}  // namespace

std::uint32_t KllSketch::required_capacity(double epsilon, double confidence) {
    if (epsilon <= 0.0 || epsilon > 1.0 || confidence <= 0.0 || confidence > 1.0) {
        throw std::invalid_argument("invalid KLL accuracy bounds");
    }

    const double delta = std::max(1.0 - confidence, 1e-12);
    const double raw_capacity = std::ceil((2.0 / epsilon) * std::sqrt(std::log(2.0 / delta)));
    const auto capacity = static_cast<std::uint64_t>(std::max<double>(kMinCapacity, raw_capacity));
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(capacity, kMaxCapacity));
}

KllSketch::KllSketch(double epsilon, double confidence, std::uint64_t seed)
    : KllSketch(required_capacity(epsilon, confidence), seed) {}

KllSketch::KllSketch(std::uint32_t capacity, std::uint64_t seed)
    : capacity_(std::clamp(capacity, kMinCapacity, kMaxCapacity)),
      seed_(seed),
      random_(splitmix64(seed)) {
    levels_.emplace_back();
    levels_.front().reserve(capacity_);
}

KllSketch::KllSketch(
    std::uint32_t capacity,
    std::uint64_t seed,
    std::uint64_t count,
    std::vector<std::vector<double>> levels)
    : capacity_(std::clamp(capacity, kMinCapacity, kMaxCapacity)),
      seed_(seed),
      count_(count),
      random_(splitmix64(seed)),
      levels_(std::move(levels)) {
    if (levels_.empty()) {
        levels_.emplace_back();
    }
}

void KllSketch::add(double value) {
    if (!std::isfinite(value)) {
        return;
    }

    levels_.front().push_back(value);
    ++count_;
    if (levels_.front().size() > capacity_) {
        compact_level(0);
    }
}

double KllSketch::quantile(double probability) const {
    if (count_ == 0) {
        return 0.0;
    }
    if (probability < 0.0 || probability > 1.0) {
        throw std::invalid_argument("quantile probability must be in [0, 1]");
    }

    std::vector<std::pair<double, std::uint64_t>> weighted_values;
    for (std::size_t level = 0; level < levels_.size(); ++level) {
        const auto weight = 1ULL << std::min<std::size_t>(level, 62);
        for (double value : levels_[level]) {
            weighted_values.emplace_back(value, weight);
        }
    }
    std::sort(weighted_values.begin(), weighted_values.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    const auto target_rank = static_cast<std::uint64_t>(
        std::ceil(probability * static_cast<double>(count_ == 0 ? 0 : count_ - 1)));
    std::uint64_t rank = 0;
    for (const auto& [value, weight] : weighted_values) {
        if (rank + weight > target_rank) {
            return value;
        }
        rank += weight;
    }

    return weighted_values.empty() ? 0.0 : weighted_values.back().first;
}

std::uint64_t KllSketch::count() const noexcept {
    return count_;
}

std::uint32_t KllSketch::capacity() const noexcept {
    return capacity_;
}

std::uint64_t KllSketch::seed() const noexcept {
    return seed_;
}

std::uint64_t KllSketch::memory_bytes() const noexcept {
    auto bytes = static_cast<std::uint64_t>(sizeof(KllSketch));
    bytes += static_cast<std::uint64_t>(levels_.capacity() * sizeof(std::vector<double>));
    for (const auto& level : levels_) {
        bytes += static_cast<std::uint64_t>(level.capacity() * sizeof(double));
    }
    return bytes;
}

const std::vector<std::vector<double>>& KllSketch::levels() const noexcept {
    return levels_;
}

void KllSketch::compact_level(std::size_t level) {
    ensure_level(level + 1);
    auto& values = levels_[level];
    std::sort(values.begin(), values.end());

    std::vector<double> retained;
    if (values.size() % 2 == 1) {
        retained.push_back(values.back());
        values.pop_back();
    }

    const std::size_t offset = sample_compaction_offset(random_);
    auto& next = levels_[level + 1];
    next.reserve(next.size() + values.size() / 2 + 1);
    for (std::size_t index = offset; index < values.size(); index += 2) {
        next.push_back(values[index]);
    }
    values = std::move(retained);

    if (next.size() > capacity_) {
        compact_level(level + 1);
    }
}

void KllSketch::ensure_level(std::size_t level) {
    while (levels_.size() <= level) {
        levels_.emplace_back();
        levels_.back().reserve(capacity_);
    }
}

}  // namespace sketchydb
