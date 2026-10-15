#pragma once

#include "fdvss/field.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace fdvss {

struct Config {
    std::size_t n = 8;
    std::size_t t = 0;
    std::size_t threads = 0;
    std::uint64_t seed = 20260923;
    bool include_final_handoff = true;
};

struct Communication {
    std::uint64_t p2p_bytes = 0;
    std::uint64_t broadcast_native_bytes = 0;
    std::uint64_t broadcast_materialized_bytes = 0;
};

struct StageSample {
    std::string protocol;
    std::string mode;
    std::string stage;
    std::size_t n = 0;
    std::size_t t = 0;
    std::size_t threads = 0;
    double milliseconds = 0.0;
    std::uint64_t peak_live_bytes = 0;
    Communication communication;
    bool verified = false;
    std::uint64_t checksum = 0;
};

struct RunRecord {
    std::string protocol;
    std::string mode;
    std::size_t n = 0;
    std::size_t t = 0;
    std::size_t threads = 0;
    std::uint64_t seed = 0;
    bool verified = false;
    std::vector<StageSample> stages;
};

class InterpolationPlan {
  public:
    explicit InterpolationPlan(std::size_t dimension);
    explicit InterpolationPlan(std::vector<Fp> points);

    [[nodiscard]] std::size_t dimension() const noexcept { return dimension_; }
    [[nodiscard]] const std::vector<Fp>& points() const noexcept { return points_; }
    [[nodiscard]] Fp basis(std::size_t coefficient, std::size_t point) const noexcept {
        return basis_[coefficient * dimension_ + point];
    }
    [[nodiscard]] Fp zero_weight(std::size_t point) const noexcept {
        return zero_weights_[point];
    }

    // values: [dimension][columns], result: [dimension coefficients][columns].
    [[nodiscard]] std::vector<Fp> interpolate_prefix(
        std::span<const Fp> values,
        std::size_t columns,
        std::size_t threads
    ) const;

    [[nodiscard]] Fp interpolate_zero(std::span<const Fp> values) const;

  private:
    std::size_t dimension_ = 0;
    std::vector<Fp> points_;
    std::vector<Fp> basis_;       // [coefficient][point]
    std::vector<Fp> zero_weights_;
};

[[nodiscard]] std::vector<Fp> evaluation_points(std::size_t count);

// coefficients: [coefficient][column], result: [point][column].
[[nodiscard]] std::vector<Fp> evaluate_matrix(
    std::span<const Fp> coefficients,
    std::size_t coefficient_count,
    std::size_t columns,
    std::span<const Fp> points,
    std::size_t threads
);

[[nodiscard]] Fp evaluate_polynomial(std::span<const Fp> coefficients, Fp point);

[[nodiscard]] RunRecord run_fdvss1_roles(const Config& config);
[[nodiscard]] RunRecord run_fdvss2(const Config& config, bool inject_fault = false);

[[nodiscard]] bool self_test(std::string* failure = nullptr);

void write_csv(const std::filesystem::path& path, const std::vector<RunRecord>& records);
void write_json(const std::filesystem::path& path, const std::vector<RunRecord>& records);

[[nodiscard]] std::string system_fingerprint();

}  // namespace fdvss
