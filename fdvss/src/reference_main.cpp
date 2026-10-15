#include "fdvss/reference.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        fdvss::Config config;
        config.n = 4;
        bool explicit_threshold = false;
        std::string protocol = "both";
        std::string scenario = "all";
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            const auto next = [&]() -> std::string {
                if (++i >= argc) throw std::invalid_argument("missing value after " + option);
                return argv[i];
            };
            if (option == "--protocol") protocol = next();
            else if (option == "--scenario") scenario = next();
            else if (option == "--n") config.n = std::stoull(next());
            else if (option == "--t") { config.t = std::stoull(next()); explicit_threshold = true; }
            else if (option == "--seed") config.seed = std::stoull(next());
            else if (option == "--help" || option == "-h") {
                std::cout << "Usage: fdvss_reference [--protocol fdvss1|fdvss2|fdpss2|both]\n"
                          << "                       [--n N] [--t T] [--seed S]\n"
                          << "                       [--scenario all|NAME]\n\n"
                          << "Connected local reference execution with explicit messages.\n"
                          << "This command is separate from the paper's timed kernels.\n";
                for (const auto& name : fdvss::reference::fdvss1_scenarios())
                    std::cout << "fdvss1: " << name << '\n';
                for (const auto& name : fdvss::reference::fdvss2_scenarios())
                    std::cout << "fdvss2: " << name << '\n';
                for (const auto& name : fdvss::reference::fdpss_scenarios())
                    std::cout << "fdpss2: " << name << '\n';
                return 0;
            } else throw std::invalid_argument("unknown option: " + option);
        }
        if (protocol != "fdvss1" && protocol != "fdvss2" && protocol != "fdpss2" && protocol != "both")
            throw std::invalid_argument("unsupported protocol");
        // The transcript retains all committee messages; use small sizes for correctness.
        if (config.n < 1 || config.n > 32)
            throw std::invalid_argument("reference execution requires 1 <= n <= 32");
        if (!explicit_threshold && config.n > 1) config.t = (config.n - 1) / 3;
        if (config.t >= config.n || config.t > (config.n - 1) / 3)
            throw std::invalid_argument("reference execution requires n > 3t");
        bool verified = true;
        bool ran = false;
        for (const std::string& p : {std::string("fdvss1"), std::string("fdvss2"), std::string("fdpss2")}) {
            if (protocol != "both" && protocol != p) continue;
            const auto scenarios = p == "fdvss1"
                ? fdvss::reference::fdvss1_scenarios()
                : p == "fdvss2" ? fdvss::reference::fdvss2_scenarios()
                                 : fdvss::reference::fdpss_scenarios();
            for (const auto& name : scenarios) {
                if (scenario != "all" && name != scenario) continue;
                if (config.t == 0 && name != "honest") {
                    if (scenario == "all") continue;
                    throw std::invalid_argument("fault scenarios require t >= 1");
                }
                ran = true;
                const auto result = p == "fdvss1"
                    ? fdvss::reference::run_fdvss1(config, name)
                    : p == "fdvss2" ? fdvss::reference::run_fdvss2(config, name)
                                     : fdvss::reference::run_fdpss(config, name);
                std::cout << p << " n=" << config.n << " t=" << config.t
                          << " scenario=" << name
                          << " qualified=" << result.qualified
                          << " verified=" << result.verified
                          << " complaints=" << result.complaints
                          << " certificates=" << result.certificates
                          << " invalid_certificates=" << result.invalid_certificates
                          << " repaired_rows=" << result.repaired_rows
                          << " decoder_failures=" << result.decoder_failures
                          << " native_payload_bytes="
                          << result.communication.p2p_bytes +
                             result.communication.broadcast_native_bytes << '\n';
                verified &= result.verified;
            }
        }
        if (!ran) throw std::invalid_argument("scenario unavailable for selected protocol");
        return verified ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
