#pragma once

#include "fdvss/core.hpp"

#include <optional>

namespace fdvss::reference {

using Polynomial = std::vector<Fp>;
using Record = std::optional<Polynomial>;

struct Decoded {
    Polynomial coefficients;  // [degree][width]
    std::vector<std::size_t> supporters;
    std::size_t width = 0;
};

// Index i denotes the nonzero field point i+1. Support is over full records.
[[nodiscard]] std::optional<Decoded> decode(
    const std::vector<Record>& values, std::size_t t, std::size_t required_support
);
[[nodiscard]] std::optional<Decoded> interpolate(
    const std::vector<Record>& values, std::size_t t
);
[[nodiscard]] Polynomial evaluate(const Decoded& polynomial, Fp point);
[[nodiscard]] Record reconstruct(
    const std::vector<Record>& values, std::size_t t, std::size_t required_support
);

struct Result {
    std::string protocol;
    std::string scenario;
    bool qualified = false;
    bool verified = false;  // Post-execution test oracle; not an acceptance rule.
    std::vector<Fp> shares;
    std::vector<std::size_t> supporters;
    std::vector<std::size_t> happy;
    std::vector<std::size_t> unhappy;
    std::size_t complaints = 0;
    std::size_t certificates = 0;
    std::size_t invalid_certificates = 0;
    std::size_t repaired_rows = 0;
    std::size_t decoder_failures = 0;
    Communication communication;  // Aggregate logical transcript payload.
};

// Connected local committee executions with explicit messages and erasures.
// Scenario injection changes sent records, never the acceptance conditions.
[[nodiscard]] std::vector<std::string> fdvss1_scenarios();
[[nodiscard]] std::vector<std::string> fdvss2_scenarios();
[[nodiscard]] std::vector<std::string> fdpss_scenarios();
[[nodiscard]] Result run_fdvss1(const Config& config, const std::string& scenario);
[[nodiscard]] Result run_fdvss2(const Config& config, const std::string& scenario);
[[nodiscard]] Result run_fdpss(const Config& config, const std::string& scenario);

}  // namespace fdvss::reference
