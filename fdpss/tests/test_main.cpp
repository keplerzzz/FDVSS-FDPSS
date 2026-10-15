#include "fdvss/core.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool has_stage(const fdvss::RunRecord& record, const std::string& name) {
    return std::any_of(record.stages.begin(), record.stages.end(), [&](const auto& stage) {
        return stage.stage == name;
    });
}

}  // namespace

int main() {
    std::string failure;
    if (!fdvss::self_test(&failure)) {
        std::cerr << failure << '\n';
        return 1;
    }
    for (const std::size_t n : {std::size_t{7}, std::size_t{10}}) {
        fdvss::Config transition;
        transition.n = n;
        transition.t = (n - 1) / 3;
        transition.threads = 4;
        transition.seed = 0x2026'0923ULL + n;
        const auto honest = fdvss::run_fdpss(transition, false);
        const auto byzantine = fdvss::run_fdpss(transition, true);
        if (!honest.verified || !byzantine.verified) {
            std::cerr << "FDPSS transition or t-error recovery failed at n=" << n << '\n';
            return 1;
        }
        if (!has_stage(honest, "rg_fdvss2_com3_batch_n")) {
            std::cerr << "FDPSS benchmark omitted the final FDVSS-2 committee at n=" << n << '\n';
            return 1;
        }
    }
    std::cout << "all tests passed\n";
    return 0;
}
