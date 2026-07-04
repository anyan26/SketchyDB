#include "hyperloglog.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace sketchydb {

namespace {

constexpr std::uint8_t kMinPrecision = 4;
constexpr std::uint8_t kMaxPrecision = 24;

std::uint64_t splitmix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::uint64_t fnv1a64(std::string_view value, std::uint64_t hash_seed) {
    std::uint64_t hash = 14695981039346656037ULL ^ splitmix64(hash_seed);
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t avalanche(std::uint64_t value) {
    value ^= value >> 33U;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33U;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33U;
    return value;
}

std::uint8_t choose_precision(double epsilon, double confidence) {
    if (epsilon <= 0.0 || epsilon > 1.0) {
        throw std::invalid_argument("epsilon must be in (0, 1]");
    }
    if (confidence <= 0.0 || confidence > 1.0) {
        throw std::invalid_argument("confidence must be in (0, 1]");
    }

    const double delta = std::max(1.0 - confidence, std::numeric_limits<double>::epsilon());
    const double target_standard_error = epsilon * std::sqrt(delta);
    const double required_registers =
        std::pow(1.04 / std::max(target_standard_error, std::numeric_limits<double>::epsilon()), 2.0);

    std::uint8_t precision = kMinPrecision;
    while (precision < kMaxPrecision && static_cast<double>(1ULL << precision) < required_registers) {
        ++precision;
    }
    return precision;
}

double alpha(std::uint32_t register_count) {
    if (register_count == 16) {
        return 0.673;
    }
    if (register_count == 32) {
        return 0.697;
    }
    if (register_count == 64) {
        return 0.709;
    }
    return 0.7213 / (1.0 + 1.079 / register_count);
}

std::uint8_t rho(std::uint64_t value, std::uint8_t max_width) {
    if (value == 0) {
        return static_cast<std::uint8_t>(max_width + 1);
    }
    const auto leading = static_cast<std::uint8_t>(__builtin_clzll(value) + 1);
    return std::min(leading, static_cast<std::uint8_t>(max_width + 1));
}

}  // namespace

std::uint8_t HyperLogLog::required_precision(double epsilon, double confidence) {
    return choose_precision(epsilon, confidence);
}

HyperLogLog::HyperLogLog(double epsilon, double confidence, std::uint64_t hash_seed)
    : HyperLogLog(choose_precision(epsilon, confidence), hash_seed) {}

HyperLogLog::HyperLogLog(std::uint8_t precision, std::uint64_t hash_seed)
    : precision_(precision),
      hash_seed_(hash_seed),
      registers_(static_cast<std::size_t>(1ULL << precision_), 0) {
    if (precision_ < kMinPrecision || precision_ > kMaxPrecision) {
        throw std::invalid_argument("invalid HyperLogLog precision");
    }
}

HyperLogLog::HyperLogLog(
    std::uint8_t precision,
    std::uint64_t hash_seed,
    std::vector<std::uint8_t> registers)
    : precision_(precision),
      hash_seed_(hash_seed),
      registers_(std::move(registers)) {
    if (precision_ < kMinPrecision || precision_ > kMaxPrecision) {
        throw std::invalid_argument("invalid HyperLogLog precision");
    }
    if (registers_.size() != static_cast<std::size_t>(1ULL << precision_)) {
        throw std::invalid_argument("register count does not match precision");
    }
}

void HyperLogLog::add(std::string_view value) {
    const auto hash = avalanche(fnv1a64(value, hash_seed_));
    const auto index = hash >> (64U - precision_);
    const auto remaining = hash << precision_;
    registers_[static_cast<std::size_t>(index)] =
        std::max(registers_[static_cast<std::size_t>(index)], rho(remaining, 64U - precision_));
}

void HyperLogLog::merge(const HyperLogLog& other) {
    if (precision_ != other.precision_) {
        throw std::invalid_argument("cannot merge HyperLogLog sketches with different precision");
    }
    if (hash_seed_ != other.hash_seed_) {
        throw std::invalid_argument("cannot merge HyperLogLog sketches with different hash seeds");
    }

    for (std::size_t index = 0; index < registers_.size(); ++index) {
        registers_[index] = std::max(registers_[index], other.registers_[index]);
    }
}

double HyperLogLog::estimate() const {
    const auto register_count_value = static_cast<double>(registers_.size());
    double harmonic_sum = 0.0;
    std::uint32_t zero_registers = 0;

    for (std::uint8_t rank : registers_) {
        harmonic_sum += std::ldexp(1.0, -static_cast<int>(rank));
        if (rank == 0) {
            ++zero_registers;
        }
    }

    double raw_estimate =
        alpha(static_cast<std::uint32_t>(registers_.size())) * register_count_value *
        register_count_value / harmonic_sum;

    if (raw_estimate <= 2.5 * register_count_value && zero_registers > 0) {
        raw_estimate = register_count_value * std::log(register_count_value / zero_registers);
    }

    return raw_estimate;
}

std::uint8_t HyperLogLog::precision() const noexcept {
    return precision_;
}

std::uint64_t HyperLogLog::hash_seed() const noexcept {
    return hash_seed_;
}

std::uint32_t HyperLogLog::register_count() const noexcept {
    return static_cast<std::uint32_t>(registers_.size());
}

const std::vector<std::uint8_t>& HyperLogLog::registers() const noexcept {
    return registers_;
}

}  // namespace sketchydb
