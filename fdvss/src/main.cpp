#include "fdvss/core.hpp"
#include "fdvss/payload.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Arguments {
    std::string protocol = "both";
    std::vector<std::size_t> sizes{8, 16, 32};
    std::size_t threshold = 0, threads = 0, warmups = 2, repeats = 10;
    std::uint64_t seed = 20260923;
    bool fixed_threshold = false, faults = false, faults_only = false, final_handoff = true;
    double max_memory_gib = 8.0;
    std::filesystem::path csv = "results/raw.csv", json = "results/raw.json";
};

std::size_t number(const std::string& text) {
    if (text.empty() || text.front() == '-') throw std::invalid_argument("expected a nonnegative integer");
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed);
    if (consumed != text.size() || value > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("invalid integer");
    return static_cast<std::size_t>(value);
}

std::vector<std::size_t> sizes(const std::string& text) {
    std::vector<std::size_t> result;
    std::size_t begin = 0;
    while (begin < text.size()) {
        const auto end = text.find(',', begin);
        const auto value = number(text.substr(begin, end == std::string::npos ? end : end - begin));
        if (value == 0 || value > 256) throw std::invalid_argument("require 1 <= n <= 256");
        result.push_back(value);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (result.empty()) throw std::invalid_argument("empty committee-size list");
    return result;
}

Arguments parse(int argc, char** argv) {
    Arguments result;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        const auto next = [&]() -> std::string {
            if (++i >= argc) throw std::invalid_argument("missing value after " + option);
            return argv[i];
        };
        if (option == "--protocol") result.protocol = next();
        else if (option == "--n") result.sizes = sizes(next());
        else if (option == "--t") { result.threshold = number(next()); result.fixed_threshold = true; }
        else if (option == "--threads") result.threads = number(next());
        else if (option == "--warmups") result.warmups = number(next());
        else if (option == "--repeats") result.repeats = number(next());
        else if (option == "--seed") result.seed = number(next());
        else if (option == "--max-memory-gib") result.max_memory_gib = std::stod(next());
        else if (option == "--csv") result.csv = next();
        else if (option == "--json") result.json = next();
        else if (option == "--faults") result.faults = true;
        else if (option == "--no-final-handoff") result.final_handoff = false;
        else if (option == "--help" || option == "-h") {
            std::cout << "Usage: fdvss_bench --protocol fdvss1|fdvss2|both\n"
                      << "  --n 8,16,32 --t T --threads N --warmups 2 --repeats 10\n"
                      << "  --seed S --csv PATH --json PATH --max-memory-gib G --faults\n";
            std::exit(0);
        } else throw std::invalid_argument("unknown option: " + option);
    }
    if (result.protocol != "fdvss1" && result.protocol != "fdvss2" && result.protocol != "both") throw std::invalid_argument("unknown protocol");
    if (result.repeats == 0 || !(result.max_memory_gib > 0) || !std::isfinite(result.max_memory_gib))
        throw std::invalid_argument("invalid run limits");
    if (result.warmups > std::numeric_limits<std::size_t>::max() - result.repeats)
        throw std::invalid_argument("run count overflow");
    return result;
}

long double memory_estimate(const std::string& protocol, std::size_t n, std::size_t t, bool final_handoff) {
    const long double count = n, d = static_cast<long double>(t) + 1;
    if (protocol == "fdvss1") {
        const auto dealer = d * d * d * d + count * d * d * d;
        const auto com3 = 3 * count * count * d + 2 * count * d * d + 2 * d * d * d;
        const auto com4 = final_handoff ? d * count * count + count * count * count : 0;
        return std::max({dealer, com3, com4}) * sizeof(fdvss::Fp) * 1.15L;
    }
    return (d * d * d + 2 * count * d * d + count * count * d + count * count)
        * sizeof(fdvss::Fp) * 1.15L;
}

fdvss::RunRecord execute(const std::string& protocol, const fdvss::Config& config, bool fault) {
    auto record = protocol == "fdvss1" ? fdvss::run_fdvss1_roles(config) : fdvss::run_fdvss2(config, fault);
    record.mode = fault ? "local_stage_kernels_fault" : "local_stage_kernels";
    for (auto& stage : record.stages) stage.mode = record.mode;
    if (!fault) {
        const std::vector<std::string> names = protocol == "fdvss1"
            ? std::vector<std::string>{"dealer", "com1", "com2", "com3_decode_and_reshare", "com4_forward", "com5_share_generation"}
            : std::vector<std::string>{"dealer", "com1_party0", "com2_party0", "com3_party0"};
        const auto payloads = fdvss::honest_payloads(protocol, config.n, config.t);
        if (payloads.size() != names.size()) throw std::logic_error("payload stage mismatch");
        for (auto& stage : record.stages) {
            const auto found = std::find(names.begin(), names.end(), stage.stage);
            if (found != names.end()) stage.communication = payloads[found - names.begin()];
        }
    }
    return record;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "self-test") {
            std::string failure;
            if (!fdvss::self_test(&failure)) throw std::runtime_error(failure);
            std::cout << "self-test passed\n";
            return 0;
        }
        const auto arguments = parse(argc, argv);
        std::vector<fdvss::RunRecord> records;
        const auto protocols = arguments.protocol == "both"
            ? std::vector<std::string>{"fdvss1", "fdvss2"}
            : std::vector<std::string>{arguments.protocol};
        for (const auto n : arguments.sizes) {
            const auto t = arguments.fixed_threshold ? arguments.threshold : (n - 1) / 3;
            if (t > (n - 1) / 3) throw std::invalid_argument("require n > 3t");
            for (const auto& protocol : protocols) {
                if (memory_estimate(protocol, n, t, arguments.final_handoff) > arguments.max_memory_gib * 1073741824.0L)
                    throw std::runtime_error("estimated memory exceeds --max-memory-gib");
                const auto modes = protocol == "fdvss2" && arguments.faults
                    ? std::vector<bool>{false, true} : std::vector<bool>{false};
                for (const bool fault : modes) {
                    for (std::size_t run = 0; run < arguments.warmups + arguments.repeats; ++run) {
                        fdvss::Config config;
                        config.n = n;
                        config.t = t;
                        config.threads = arguments.threads;
                        config.seed = arguments.seed + 0x100000ULL * n + 97ULL * run + (fault ? 1 : 0);
                        config.include_final_handoff = arguments.final_handoff;
                        auto record = execute(protocol, config, fault);
                        if (!record.verified) throw std::runtime_error("kernel verification failed");
                        if (run >= arguments.warmups) records.push_back(std::move(record));
                    }
                }
                std::cout << protocol << " n=" << n << " complete\n";
            }
        }
        fdvss::write_csv(arguments.csv, records);
        fdvss::write_json(arguments.json, records);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
