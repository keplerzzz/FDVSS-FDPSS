#include "fdvss/reference.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("FDVSS-1 reference: " + message);
}

std::vector<std::size_t> prefix(std::size_t count) {
    std::vector<std::size_t> indices(count);
    std::iota(indices.begin(), indices.end(), 0);
    return indices;
}

bool zero_shares(const fdvss::reference::Result& result) {
    return std::all_of(result.shares.begin(), result.shares.end(),
                       [](fdvss::Fp share) { return share.is_zero(); });
}

}  // namespace

void fdvss1_reference_tests() {
    using namespace fdvss;
    using namespace fdvss::reference;
    for (const auto [n, t] : {std::pair<std::size_t, std::size_t>{4, 1}, {7, 2}, {8, 2}}) {
        Config config;
        config.n = n;
        config.t = t;
        config.seed = 20261007 + n;
        for (const auto& scenario : fdvss1_scenarios()) {
            const auto result = run_fdvss1(config, scenario);
            const std::string context = scenario + " n=" + std::to_string(n);
            require(result.verified, context + " did not match its post-execution oracle");
            require(result.shares.size() == n, context + " missing final receivers");
            require(result.communication.p2p_bytes > 0, context + " missing private transcript");
            require(result.communication.broadcast_native_bytes > 0, context + " missing broadcasts");
            require(result.communication.broadcast_materialized_bytes
                    == n * result.communication.broadcast_native_bytes,
                    context + " inconsistent materialized broadcast count");
            if (scenario == "inconsistent-dealer" || scenario == "decoder-failure"
                || scenario == "repair-decoder-failure-diagnostic"
                || scenario == "status-quorum-failure-diagnostic") {
                require(!result.qualified && zero_shares(result), context + " missing common zero default");
                require(result.decoder_failures > 0, context + " did not execute a failure branch");
            } else {
                require(result.qualified, context + " should be qualified");
                require(result.supporters.size() >= n - t, context + " insufficient complete supporters");
            }
            if (scenario == "honest") {
                require(result.supporters == prefix(n), context + " truncated S to n-t");
                require(result.happy == prefix(n) && result.unhappy.empty(), context + " incorrect classification");
                require(result.complaints == 0 && result.decoder_failures == 0, context + " spurious failure");
            }
            if (scenario == "bad-row" || scenario == "combined" || scenario == "bad-com3-values") {
                require(result.unhappy == prefix(t), context + " did not select every corrupt Com_1 row");
                require(result.repaired_rows == t, context + " did not perform public repair");
                require(result.happy.size() == n - t, context + " wrong happy row count");
            }
            if (scenario == "bad-row") {
                require(result.supporters == prefix(n), context + " should preserve all Com_3 supporters");
                require(result.complaints == n * t, context + " missing column complaints");
            }
            if (scenario == "malformed-row") {
                require(result.unhappy == prefix(1) && result.repaired_rows == 1,
                        context + " invalid dealer row did not reach zero fallback and repair");
            }
            if (scenario == "all-malformed-rows") {
                require(result.qualified && zero_shares(result), context + " zero-row commitment should qualify");
                require(result.unhappy.empty() && result.supporters == prefix(n), context + " invalid zero commitment");
            }
            if (scenario == "bad-com2") {
                require(result.complaints == n * t, context + " malformed complaint was not normalized");
                require(result.happy == prefix(n), context + " corrupt columns changed honest classification");
                require(result.decoder_failures > 0, context + " omitted raw values did not reach qhat failure");
            }
            if (scenario == "bad-com3" || scenario == "combined") {
                std::vector<std::size_t> expected_supporters;
                for (std::size_t k = t; k < n; ++k) expected_supporters.push_back(k);
                require(result.supporters == expected_supporters, context + " erased complete vectors still in S");
                require(result.decoder_failures > 0, context + " failed inner shares were not erasures");
            }
            if (scenario == "bad-com3-values") {
                std::vector<std::size_t> expected_supporters;
                for (std::size_t k = t; k < n; ++k) expected_supporters.push_back(k);
                require(result.supporters == expected_supporters, context + " wrong complete vectors still in S");
                require(result.decoder_failures == 0, context + " within-budget finite errors should decode");
            }
            if (scenario == "bad-com4") {
                require(result.supporters == prefix(n), context + " corrupt status changed S");
                require(result.happy == prefix(n), context + " corrupt private forwarding changed classification");
            }
            if (scenario == "repair-decoder-failure-diagnostic") {
                require(result.unhappy == prefix(t), context + " diagnostic did not first create a genuine repair");
                require(result.repaired_rows == 0, context + " insufficient repair quorum unexpectedly decoded");
            }
        }
    }
    Config no_corruption;
    no_corruption.n = 4;
    no_corruption.t = 0;
    const auto trivial = run_fdvss1(no_corruption, "honest");
    require(trivial.verified && trivial.qualified && trivial.supporters == prefix(4), "t=0 execution");
    Config invalid;
    invalid.n = 6;
    invalid.t = 2;
    bool rejected = false;
    try { (void)run_fdvss1(invalid, "honest"); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "n <= 3t was accepted");
}
