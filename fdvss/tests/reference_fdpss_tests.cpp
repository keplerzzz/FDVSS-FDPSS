#include "fdvss/reference.hpp"

#include <cmath>
#include <stdexcept>

void fdpss_reference_tests() {
    for (const std::size_t n : {4U, 7U, 8U}) {
        fdvss::Config config;
        config.n = n;
        config.t = (n - 1) / 3;
        for (const auto& scenario : fdvss::reference::fdpss_scenarios()) {
            const auto result = fdvss::reference::run_fdpss(config, scenario);
            if (!result.verified) throw std::runtime_error("FDPSS reference: " + scenario);
            if (result.stages.size() != 3 || result.stages[0].stage != "rg" ||
                result.stages[1].stage != "rg_vandermonde" || result.stages[2].stage != "rd")
                throw std::runtime_error("FDPSS measurement stages are incomplete");
            double elapsed = 0.0;
            fdvss::Communication communication;
            for (const auto& stage : result.stages) {
                if (!stage.verified || stage.milliseconds < 0.0)
                    throw std::runtime_error("invalid FDPSS measurement stage");
                elapsed += stage.milliseconds;
                communication.p2p_bytes += stage.communication.p2p_bytes;
                communication.broadcast_native_bytes += stage.communication.broadcast_native_bytes;
                communication.broadcast_materialized_bytes += stage.communication.broadcast_materialized_bytes;
            }
            if (std::abs(elapsed - result.total_milliseconds) > 1e-6 ||
                communication.p2p_bytes != result.communication.p2p_bytes ||
                communication.broadcast_native_bytes != result.communication.broadcast_native_bytes ||
                communication.broadcast_materialized_bytes != result.communication.broadcast_materialized_bytes)
                throw std::runtime_error("FDPSS stage totals disagree");
            const auto scalar_record_bytes = sizeof(std::uint8_t) + 2 * sizeof(std::uint64_t);
            const auto missing = scenario == "missing-rd" ? config.t :
                (scenario == "rd-decoder-failure-diagnostic" ? config.t + 1 : 0);
            const auto rd_bytes = n * ((n - missing) * scalar_record_bytes + missing * sizeof(std::uint8_t));
            if (result.stages[2].communication.p2p_bytes != rd_bytes)
                throw std::runtime_error("RD serialization count disagrees");
            if (scenario == "bad-rg-dealer" || scenario == "combined") {
                if (result.certificates == 0)
                    throw std::runtime_error("RG dealer rejection did not produce certificates");
            }
            if (scenario == "rd-decoder-failure-diagnostic") {
                if (result.qualified || result.decoder_failures < n)
                    throw std::runtime_error("RD diagnostic did not trigger default path");
            } else if (!result.qualified) throw std::runtime_error("valid RD transition rejected");
        }
    }
}
