#include "fdvss/reference.hpp"

#include <stdexcept>

void fdpss_reference_tests() {
    for (const std::size_t n : {4U, 7U, 8U}) {
        fdvss::Config config;
        config.n = n;
        config.t = (n - 1) / 3;
        for (const auto& scenario : fdvss::reference::fdpss_scenarios()) {
            const auto result = fdvss::reference::run_fdpss(config, scenario);
            if (!result.verified) throw std::runtime_error("FDPSS reference: " + scenario);
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
