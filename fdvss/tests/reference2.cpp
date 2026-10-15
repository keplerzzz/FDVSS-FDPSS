#include "fdvss/reference.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void check_scenario(std::size_t n, std::size_t t, const std::string& scenario) {
    fdvss::Config config;
    config.n = n;
    config.t = t;
    config.seed = 20261007 + n;
    const auto result = fdvss::reference::run_fdvss2(config, scenario);
    require(result.verified, "FDVSS-2 " + scenario + " failed its post-execution oracle");
    require(result.shares.size() == n, "FDVSS-2 output dimension mismatch");
    require(result.communication.p2p_bytes != 0 && result.communication.broadcast_native_bytes != 0,
            "FDVSS-2 explicit transcript accounting is empty");
    require(result.communication.broadcast_materialized_bytes == n * result.communication.broadcast_native_bytes,
            "FDVSS-2 native/materialized broadcast accounting mismatch");

    const bool reject = scenario == "inconsistent-dealer" || scenario == "table-no-quorum" ||
                        scenario == "malformed-dealer-table";
    require(result.qualified != reject, "FDVSS-2 " + scenario + " qualification mismatch");
    if (reject) {
        require(std::all_of(result.shares.begin(), result.shares.end(), [](auto value) { return value.is_zero(); }),
                "FDVSS-2 disqualification did not select the common zero sharing");
    }
    if (scenario == "inconsistent-dealer") {
        require(result.certificates == n, "FDVSS-2 inconsistent rows did not produce authenticated witnesses");
        require(result.invalid_certificates == 0, "FDVSS-2 rejected an authentic inconsistency witness");
    }
    if (scenario == "forged-certificate" || scenario == "duplicate-certificate" ||
        scenario == "malformed-certificate" || scenario == "consistent-certificate" ||
        scenario == "excluded-sender-certificate") {
        require(result.certificates == 1 && result.invalid_certificates == 1,
                "FDVSS-2 failed to ignore one invalid certificate");
    }
    if (scenario == "invalid-slice" || scenario == "missing-slice" || scenario == "malformed-slice") {
        require(result.complaints == n * t, "FDVSS-2 did not erase every faulty sender's slice");
        require(result.certificates == 0, "FDVSS-2 unauthenticated records entered a witness");
    }
    if (scenario == "insufficient-records-diagnostic") {
        require(result.decoder_failures == 1, "FDVSS-2 insufficient records did not take the zero-forwarding branch");
    }
}

}  // namespace

void fdvss2_reference_tests() {
    for (const auto n : {std::size_t{4}, std::size_t{7}, std::size_t{8}}) {
        const auto t = (n - 1) / 3;
        for (const auto& scenario : fdvss::reference::fdvss2_scenarios()) check_scenario(n, t, scenario);
    }
    // t=0 uses no Byzantine senders; singleton honest sharing is still valid.
    fdvss::Config singleton;
    singleton.n = 1;
    singleton.t = 0;
    require(fdvss::reference::run_fdvss2(singleton, "honest").verified, "FDVSS-2 t=0 failed");
    singleton.n = 2;
    const auto zero_degree_fault = fdvss::reference::run_fdvss2(singleton, "inconsistent-dealer");
    require(zero_degree_fault.verified && !zero_degree_fault.qualified && zero_degree_fault.certificates == 2,
            "FDVSS-2 t=0 authentic inconsistency did not disqualify");

    // Reproducible test data is isolated by the protocol instance seed.
    fdvss::Config config;
    config.n = 4;
    config.t = 1;
    const auto first = fdvss::reference::run_fdvss2(config, "honest");
    const auto repeat = fdvss::reference::run_fdvss2(config, "honest");
    require(first.shares == repeat.shares, "FDVSS-2 reference seed is not reproducible");
    ++config.seed;
    const auto independent = fdvss::reference::run_fdvss2(config, "honest");
    require(first.shares != independent.shares, "FDVSS-2 independent instances reused dealer input");

    bool rejected = false;
    try {
        config.n = 3;
        config.t = 1;
        static_cast<void>(fdvss::reference::run_fdvss2(config, "honest"));
    } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "FDVSS-2 accepted n <= 3t");
    rejected = false;
    try {
        config.n = 4;
        static_cast<void>(fdvss::reference::run_fdvss2(config, "unknown-scenario"));
    } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "FDVSS-2 accepted an unknown fault scenario");
}
