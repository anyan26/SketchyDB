#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sketchydb {

struct TopKItem {
    std::string value;
    std::uint64_t estimate = 0;
    std::uint64_t error = 0;
};

class CountMinSketch {
public:
    static std::uint32_t required_width(double epsilon);
    static std::uint32_t required_depth(double confidence);

    CountMinSketch(double epsilon, double confidence, std::uint64_t seed);
    CountMinSketch(std::uint32_t width, std::uint32_t depth, std::uint64_t seed);
    CountMinSketch(std::uint32_t width, std::uint32_t depth, std::uint64_t seed, std::vector<std::uint64_t> counters);

    void add(std::string_view value, std::uint64_t count = 1);
    [[nodiscard]] std::uint64_t estimate(std::string_view value) const;
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t depth() const noexcept;
    [[nodiscard]] std::uint64_t seed() const noexcept;
    [[nodiscard]] std::uint64_t memory_bytes() const noexcept;
    [[nodiscard]] const std::vector<std::uint64_t>& counters() const noexcept;

private:
    [[nodiscard]] std::uint64_t hash(std::string_view value, std::uint32_t row) const;

    std::uint32_t width_ = 0;
    std::uint32_t depth_ = 0;
    std::uint64_t seed_ = 0;
    std::vector<std::uint64_t> counters_;
};

class SpaceSavingTopK {
public:
    explicit SpaceSavingTopK(std::uint32_t capacity);
    SpaceSavingTopK(std::uint32_t capacity, std::vector<TopKItem> items);

    void add(std::string_view value);
    [[nodiscard]] std::vector<TopKItem> top_k(std::uint32_t k) const;
    [[nodiscard]] std::uint32_t capacity() const noexcept;
    [[nodiscard]] std::uint64_t memory_bytes() const noexcept;

private:
    struct Counter {
        std::uint64_t estimate = 0;
        std::uint64_t error = 0;
    };

    std::uint32_t capacity_ = 0;
    std::unordered_map<std::string, Counter> counters_;
};

}  // namespace sketchydb
