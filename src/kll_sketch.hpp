#pragma once

#include <cstdint>
#include <random>
#include <vector>

namespace sketchydb {

class KllSketch {
public:
    static std::uint32_t required_capacity(double epsilon, double confidence);

    KllSketch(double epsilon, double confidence, std::uint64_t seed);
    KllSketch(std::uint32_t capacity, std::uint64_t seed);

    void add(double value);
    [[nodiscard]] double quantile(double probability) const;
    [[nodiscard]] std::uint64_t count() const noexcept;
    [[nodiscard]] std::uint32_t capacity() const noexcept;
    [[nodiscard]] std::uint64_t seed() const noexcept;
    [[nodiscard]] std::uint64_t memory_bytes() const noexcept;

private:
    void compact_level(std::size_t level);
    void ensure_level(std::size_t level);

    std::uint32_t capacity_ = 0;
    std::uint64_t seed_ = 0;
    std::uint64_t count_ = 0;
    mutable std::mt19937_64 random_;
    std::vector<std::vector<double>> levels_;
};

}  // namespace sketchydb
