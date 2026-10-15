#include "fdvss/core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Arguments {
    std::string protocol = "both";
    std::vector<std::size_t> sizes{8, 16, 32};
    std::size_t threshold = 0;
    std::size_t threads = 0;
    std::size_t warmups = 2;
    std::size_t repeats = 10;
    std::uint64_t seed = 20260923;
    bool faults = false;
    bool final_handoff = true;
    double max_memory_gib = 8.0;
    std::filesystem::path csv = "results/raw.csv";
    std::filesystem::path json = "results/raw.json";
};

std::vector<std::size_t> parse_sizes(const std::string& value) {
    std::vector<std::size_t> result;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t comma = value.find(',', begin);
        const std::string token = value.substr(begin, comma == std::string::npos ? comma : comma - begin);
        const auto parsed = static_cast<std::size_t>(std::stoull(token));
        if (parsed == 0) throw std::invalid_argument("n must be positive");
        result.push_back(parsed);
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    if (result.empty()) throw std::invalid_argument("empty n list");
    return result;
}

Arguments parse_arguments(int argc, char** argv) {
    Arguments result;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        const auto next = [&]() -> std::string {
            if (++i >= argc) throw std::invalid_argument("missing value after " + option);
            return argv[i];
        };
        if (option == "--protocol") result.protocol = next();
        else if (option == "--n") result.sizes = parse_sizes(next());
        else if (option == "--t") result.threshold = static_cast<std::size_t>(std::stoull(next()));
        else if (option == "--threads") result.threads = static_cast<std::size_t>(std::stoull(next()));
        else if (option == "--warmups") result.warmups = static_cast<std::size_t>(std::stoull(next()));
        else if (option == "--repeats") result.repeats = static_cast<std::size_t>(std::stoull(next()));
        else if (option == "--seed") result.seed = std::stoull(next());
        else if (option == "--csv") result.csv = next();
        else if (option == "--json") result.json = next();
        else if (option == "--max-memory-gib") result.max_memory_gib = std::stod(next());
        else if (option == "--faults") result.faults = true;
        else if (option == "--no-final-handoff") result.final_handoff = false;
        else if (option == "--help" || option == "-h") {
            std::cout
                << "Usage: fdvss_bench [options]\n\n"
                << "  --protocol fdvss1|fdvss2|both\n"
                << "  --n 8,16,32             committee sizes\n"
                << "  --t N                   fixed threshold (default: floor((n-1)/3))\n"
                << "  --threads N             0 uses hardware concurrency\n"
                << "  --warmups N             discarded runs per size/mode\n"
                << "  --repeats N             recorded runs per size/mode\n"
                << "  --faults                also run FDVSS-2 invalid-record path\n"
                << "  --no-final-handoff      omit FDVSS-1 Com4/Com5 kernels\n"
                << "  --max-memory-gib G      refuse estimated peaks above G GiB\n"
                << "  --csv PATH --json PATH  raw output paths\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    if (result.protocol != "fdvss1" && result.protocol != "fdvss2" && result.protocol != "both") {
        throw std::invalid_argument("--protocol must be fdvss1, fdvss2, or both");
    }
    if (result.repeats == 0) throw std::invalid_argument("--repeats must be positive");
    if (!(result.max_memory_gib > 0.0)) throw std::invalid_argument("--max-memory-gib must be positive");
    return result;
}

long double power(long double value, unsigned exponent) {
    long double result = 1.0L;
    for (unsigned i = 0; i < exponent; ++i) result *= value;
    return result;
}

long double estimated_peak_bytes(
    const std::string& protocol,
    std::size_t n,
    std::size_t t,
    bool final_handoff
) {
    const long double nd = n;
    const long double d = t + 1;
    long double elements = 0;
    if (protocol == "fdvss1") {
        const long double dealer = power(d, 4) + nd * power(d, 3);
        const long double com3 = nd * nd * d + 2 * nd * d * d + 2 * power(d, 3) + 2 * nd * nd * d;
        const long double com4 = final_handoff ? d * nd * nd + power(nd, 3) : 0;
        elements = std::max({dealer, com3, com4});
    } else {
        elements = power(d, 3) + nd * d * d + nd * nd * d + nd * d * d + nd * nd;
    }
    return elements * sizeof(fdvss::Fp) * 1.15L;
}

double mean(const std::vector<double>& values) {
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

void print_summary(const std::vector<fdvss::RunRecord>& records) {
    using Key = std::tuple<std::string, std::string, std::size_t, std::string>;
    std::map<Key, std::vector<double>> samples;
    std::map<Key, const fdvss::StageSample*> metadata;
    for (const auto& record : records) {
        for (const auto& stage : record.stages) {
            Key key{record.protocol, record.mode, record.n, stage.stage};
            samples[key].push_back(stage.milliseconds);
            metadata[key] = &stage;
        }
    }
    std::cout << "\nprotocol mode n stage mean_ms p2p_MiB broadcast_native_MiB verified\n";
    std::cout << std::fixed << std::setprecision(3);
    for (const auto& [key, timings] : samples) {
        const auto& [protocol, mode, n, stage] = key;
        const auto* info = metadata.at(key);
        std::cout << protocol << ' ' << mode << ' ' << n << ' ' << stage << ' '
                  << mean(timings) << ' '
                  << static_cast<double>(info->communication.p2p_bytes) / (1024.0 * 1024.0) << ' '
                  << static_cast<double>(info->communication.broadcast_native_bytes) / (1024.0 * 1024.0) << ' '
                  << (info->verified ? "yes" : "NO") << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::string(argv[1]) == "self-test") {
            std::string failure;
            if (!fdvss::self_test(&failure)) {
                std::cerr << "self-test failed: " << failure << '\n';
                return 1;
            }
            std::cout << "self-test passed\n";
            return 0;
        }

        const Arguments arguments = parse_arguments(argc, argv);
        std::string failure;
        if (!fdvss::self_test(&failure)) {
            throw std::runtime_error("pre-benchmark self-test failed: " + failure);
        }
        std::cout << "system: " << fdvss::system_fingerprint() << '\n';

        std::vector<fdvss::RunRecord> records;
        bool all_verified = true;
        const long double memory_limit = arguments.max_memory_gib * 1024.0L * 1024.0L * 1024.0L;

        for (const std::size_t n : arguments.sizes) {
            const std::size_t t = arguments.threshold == 0 ? (n - 1) / 3 : arguments.threshold;
            const std::vector<std::string> protocols = arguments.protocol == "both"
                ? std::vector<std::string>{"fdvss1", "fdvss2"}
                : std::vector<std::string>{arguments.protocol};
            for (const std::string& protocol : protocols) {
                const long double estimated = estimated_peak_bytes(protocol, n, t, arguments.final_handoff);
                if (estimated > memory_limit) {
                    std::ostringstream message;
                    message << "estimated peak for " << protocol << " n=" << n << " is "
                            << static_cast<double>(estimated / (1024.0L * 1024.0L * 1024.0L))
                            << " GiB, above --max-memory-gib";
                    throw std::runtime_error(message.str());
                }

                const std::size_t modes = protocol == "fdvss2" && arguments.faults ? 2 : 1;
                for (std::size_t mode_index = 0; mode_index < modes; ++mode_index) {
                    const bool fault = mode_index == 1;
                    for (std::size_t run = 0; run < arguments.warmups + arguments.repeats; ++run) {
                        fdvss::Config config;
                        config.n = n;
                        config.t = t;
                        config.threads = arguments.threads;
                        config.seed = arguments.seed + 0x100000ULL * n + 97ULL * run + mode_index;
                        config.include_final_handoff = arguments.final_handoff;
                        fdvss::RunRecord result = protocol == "fdvss1"
                            ? fdvss::run_fdvss1_roles(config)
                            : fdvss::run_fdvss2(config, fault);
                        std::cout << protocol << " n=" << n << " mode=" << result.mode
                                  << " run=" << run << " verified=" << (result.verified ? "yes" : "NO") << '\n';
                        if (run >= arguments.warmups) {
                            all_verified &= result.verified;
                            records.push_back(std::move(result));
                        }
                    }
                }
            }
        }

        fdvss::write_csv(arguments.csv, records);
        fdvss::write_json(arguments.json, records);
        print_summary(records);
        std::cout << "\nraw CSV: " << arguments.csv << "\nraw JSON: " << arguments.json << '\n';
        return all_verified ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}

