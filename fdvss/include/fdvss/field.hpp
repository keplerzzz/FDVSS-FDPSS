#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace fdvss {

// Goldilocks prime p = 2^64 - 2^32 + 1. Values are always canonical.
class Fp {
  public:
    static constexpr std::uint64_t modulus = 0xffff'ffff'0000'0001ULL;
    static constexpr std::uint64_t epsilon = 0x0000'0000'ffff'ffffULL;

    constexpr Fp() noexcept = default;

    static constexpr Fp from_u64(std::uint64_t value) noexcept {
        return value >= modulus ? Fp(value - modulus, RawTag{}) : Fp(value, RawTag{});
    }
    static constexpr Fp zero() noexcept { return Fp(0, RawTag{}); }
    static constexpr Fp one() noexcept { return Fp(1, RawTag{}); }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }

    friend constexpr bool operator==(Fp lhs, Fp rhs) noexcept = default;

    friend constexpr Fp operator+(Fp lhs, Fp rhs) noexcept {
        const std::uint64_t sum = lhs.value_ + rhs.value_;
        if (sum < lhs.value_) {
            return Fp(sum + epsilon, RawTag{});
        }
        return Fp(sum >= modulus ? sum - modulus : sum, RawTag{});
    }

    friend constexpr Fp operator-(Fp lhs, Fp rhs) noexcept {
        if (lhs.value_ >= rhs.value_) {
            return Fp(lhs.value_ - rhs.value_, RawTag{});
        }
        return Fp(modulus - (rhs.value_ - lhs.value_), RawTag{});
    }

    friend inline Fp operator*(Fp lhs, Fp rhs) noexcept {
        return reduce128(static_cast<unsigned __int128>(lhs.value_) * rhs.value_);
    }

    Fp& operator+=(Fp rhs) noexcept {
        *this = *this + rhs;
        return *this;
    }
    Fp& operator-=(Fp rhs) noexcept {
        *this = *this - rhs;
        return *this;
    }
    Fp& operator*=(Fp rhs) noexcept {
        *this = *this * rhs;
        return *this;
    }

    [[nodiscard]] Fp pow(std::uint64_t exponent) const noexcept {
        Fp base = *this;
        Fp result = one();
        while (exponent != 0) {
            if ((exponent & 1U) != 0) {
                result *= base;
            }
            base *= base;
            exponent >>= 1U;
        }
        return result;
    }

    [[nodiscard]] Fp inverse() const {
        if (is_zero()) {
            throw std::domain_error("zero has no inverse");
        }
        return pow(modulus - 2);
    }

  private:
    struct RawTag {};
    std::uint64_t value_ = 0;

    constexpr Fp(std::uint64_t value, RawTag) noexcept : value_(value) {}

    static inline Fp reduce128(unsigned __int128 value) noexcept {
        const auto low = static_cast<std::uint64_t>(value);
        const auto high = static_cast<std::uint64_t>(value >> 64U);
        const auto high_high = high >> 32U;
        const auto high_low = high & epsilon;

        std::uint64_t t0 = low - high_high;
        if (low < high_high) {
            t0 -= epsilon;
        }
        const std::uint64_t t1 = high_low * epsilon;
        std::uint64_t reduced = t0 + t1;
        if (reduced < t0) {
            reduced += epsilon;
        }
        if (reduced >= modulus) {
            reduced -= modulus;
        }
        return Fp(reduced, RawTag{});
    }
};

class SplitMix64 {
  public:
    explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next_u64() noexcept {
        std::uint64_t z = (state_ += 0x9e37'79b9'7f4a'7c15ULL);
        z = (z ^ (z >> 30U)) * 0xbf58'476d'1ce4'e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d0'49bb'1331'11ebULL;
        return z ^ (z >> 31U);
    }

    Fp next_field() noexcept {
        for (;;) {
            const std::uint64_t candidate = next_u64();
            if (candidate < Fp::modulus) {
                return Fp::from_u64(candidate);
            }
        }
    }

  private:
    std::uint64_t state_;
};

inline std::vector<Fp> batch_inverse(const std::vector<Fp>& values) {
    std::vector<Fp> prefix(values.size(), Fp::one());
    Fp product = Fp::one();
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i].is_zero()) {
            throw std::domain_error("batch inversion contains zero");
        }
        prefix[i] = product;
        product *= values[i];
    }
    Fp suffix = product.inverse();
    std::vector<Fp> result(values.size());
    for (std::size_t i = values.size(); i-- > 0;) {
        result[i] = prefix[i] * suffix;
        suffix *= values[i];
    }
    return result;
}

}  // namespace fdvss

