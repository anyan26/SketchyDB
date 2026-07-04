#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace sketchydb {

class HyperLogLog {
public:
    static std::uint8_t required_precision(double epsilon, double confidence);

    HyperLogLog(double epsilon, double confidence, std::uint64_t hash_seed);
    HyperLogLog(std::uint8_t precision, std::uint64_t hash_seed);
    HyperLogLog(std::uint8_t precision, std::uint64_t hash_seed, std::vector<std::uint8_t> registers);

    void add(std::string_view value);
    void merge(const HyperLogLog& other);
    [[nodiscard]] double estimate() const;
    [[nodiscard]] std::uint8_t precision() const noexcept;
    [[nodiscard]] std::uint64_t hash_seed() const noexcept;
    [[nodiscard]] std::uint32_t register_count() const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>& registers() const noexcept;

private:
    std::uint8_t precision_ = 0;
    std::uint64_t hash_seed_ = 0;
    std::vector<std::uint8_t> registers_;
};

}  // namespace sketchydb
