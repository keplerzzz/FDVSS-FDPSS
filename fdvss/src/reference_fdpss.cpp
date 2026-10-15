#include "fdvss/reference.hpp"

#include <algorithm>
#include <stdexcept>

namespace fdvss::reference {

std::vector<std::string> fdpss_scenarios() {
    return {"honest", "bad-rg-dealer", "bad-rd", "missing-rd", "combined",
            "rd-decoder-failure-diagnostic"};
}

Result run_fdpss(const Config& config, const std::string& scenario) {
    const auto scenarios = fdpss_scenarios();
    if (std::find(scenarios.begin(), scenarios.end(), scenario) == scenarios.end())
        throw std::invalid_argument("unknown FDPSS reference scenario");
    const std::size_t n = config.n;
    const std::size_t t = config.t;
    if (n == 0 || t > (n - 1) / 3)
        throw std::invalid_argument("FDPSS reference requires n > 3t");
    Result result;
    result.protocol = "fdpss2";
    result.scenario = scenario;
    result.shares.assign(n, Fp::zero());

    // Connected RG: each dealer's actual FDVSS output feeds the transform.
    // No independent output-contract harness substitutes for these shares.
    std::vector<std::vector<Fp>> inputs(n);
    std::vector<Fp> input_secrets(n);
    bool inputs_valid = true;
    const bool faulty_dealers = scenario == "bad-rg-dealer" || scenario == "combined";
    for (std::size_t dealer = 0; dealer < n; ++dealer) {
        Config instance = config;
        instance.seed = config.seed + 0x9e3779b97f4a7c15ULL * (dealer + 1);
        const auto sharing = reference::run_fdvss2(
            instance, faulty_dealers && dealer < t ? "inconsistent-dealer" : "honest"
        );
        inputs[dealer] = sharing.shares;
        result.certificates += sharing.certificates;
        result.invalid_certificates += sharing.invalid_certificates;
        result.decoder_failures += sharing.decoder_failures;
        result.communication.p2p_bytes += sharing.communication.p2p_bytes;
        result.communication.broadcast_native_bytes += sharing.communication.broadcast_native_bytes;
        result.communication.broadcast_materialized_bytes += sharing.communication.broadcast_materialized_bytes;
        std::vector<Record> records(n);
        for (std::size_t party = 0; party < n; ++party)
            records[party] = Polynomial{inputs[dealer][party]};
        // This is a post-execution validity oracle, not an RG protocol step.
        const auto recovered = reconstruct(records, t, n);
        inputs_valid &= recovered.has_value();
        if (recovered) input_secrets[dealer] = (*recovered)[0];
        inputs_valid &= sharing.verified;
    }
    const std::size_t count = n - t;
    std::vector<Polynomial> random_shares(count, Polynomial(n, Fp::zero()));
    Polynomial random_secrets(count, Fp::zero());
    for (std::size_t dealer = 0; dealer < n; ++dealer) {
        const Fp point = Fp::from_u64(dealer + 1);
        Fp power = Fp::one();
        for (std::size_t coefficient = 0; coefficient < count; ++coefficient) {
            for (std::size_t party = 0; party < n; ++party)
                random_shares[coefficient][party] += power * inputs[dealer][party];
            random_secrets[coefficient] += power * input_secrets[dealer];
            power *= point;
        }
    }

    // Valid old sharing is the input contract of a steady-state transition.
    SplitMix64 random(config.seed ^ 0x52442d524546ULL);
    Polynomial old_coefficients(t + 1);
    for (auto& value : old_coefficients) value = random.next_field();
    const Fp secret = old_coefficients[0];
    std::vector<std::vector<Record>> incoming(n, std::vector<Record>(n));
    for (std::size_t source = 0; source < n; ++source) {
        const Fp old_share = evaluate_polynomial(old_coefficients, Fp::from_u64(source + 1));
        for (std::size_t target = 0; target < n; ++target) {
            const Fp point = Fp::from_u64(target + 1);
            Fp value = old_share;
            Fp power = point;
            for (std::size_t coefficient = 0; coefficient < t; ++coefficient) {
                value += random_shares[coefficient][source] * power;
                power *= point;
            }
            incoming[target][source] = Polynomial{value};
            if ((scenario == "bad-rd" || scenario == "combined") && source < t)
                (*incoming[target][source])[0] += Fp::from_u64(101 + target + source);
            if (scenario == "missing-rd" && source < t) incoming[target][source].reset();
            // Deliberately exceeds the channel-fault budget to exercise failure.
            if (scenario == "rd-decoder-failure-diagnostic" && source <= t)
                incoming[target][source].reset();
            result.communication.p2p_bytes += incoming[target][source] ? 17 : 1;
        }
    }
    bool all_reconstructed = true;
    bool matches = true;
    for (std::size_t target = 0; target < n; ++target) {
        const auto recovered = reconstruct(incoming[target], t, n - t);
        if (!recovered) {
            ++result.decoder_failures;
            all_reconstructed = false;
        } else result.shares[target] = (*recovered)[0];
        // Expected degree-t output, computed only for verification.
        const Fp point = Fp::from_u64(target + 1);
        Fp expected = secret;
        Fp power = point;
        for (std::size_t coefficient = 0; coefficient < t; ++coefficient) {
            expected += random_secrets[coefficient] * power;
            power *= point;
        }
        matches &= result.shares[target] == expected;
    }
    result.qualified = all_reconstructed;
    const bool failure_diagnostic = scenario == "rd-decoder-failure-diagnostic";
    result.verified = inputs_valid && (failure_diagnostic
        ? !all_reconstructed && std::all_of(result.shares.begin(), result.shares.end(),
                                            [](Fp value) { return value.is_zero(); })
        : all_reconstructed && matches);
    return result;
}

}  // namespace fdvss::reference
