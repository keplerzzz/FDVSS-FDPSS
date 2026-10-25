#include "fdvss/reference.hpp"
#include "fdvss/measurement.hpp"

#include <algorithm>
#include <stdexcept>

namespace fdvss::reference {
namespace {

using Bytes = std::vector<std::uint8_t>;

void append_integer(Bytes& bytes, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i)
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

Bytes encode_record(const Record& record) {
    Bytes bytes{static_cast<std::uint8_t>(record.has_value())};
    if (record) {
        append_integer(bytes, record->size());
        for (const auto value : *record) append_integer(bytes, value.value());
    }
    return bytes;
}

Record parse_record(const Bytes& bytes) {
    if (bytes.empty() || bytes[0] != 1) return std::nullopt;
    std::size_t position = 1;
    auto integer = [&]() -> std::optional<std::uint64_t> {
        if (bytes.size() - position < 8) return std::nullopt;
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i)
            value |= static_cast<std::uint64_t>(bytes[position++]) << (8 * i);
        return value;
    };
    const auto size = integer();
    if (!size || *size != 1) return std::nullopt;
    Polynomial record;
    for (std::size_t i = 0; i < *size; ++i) {
        const auto value = integer();
        if (!value || *value >= Fp::modulus) return std::nullopt;
        record.push_back(Fp::from_u64(*value));
    }
    return position == bytes.size() ? Record(std::move(record)) : std::nullopt;
}

}  // namespace

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

    SplitMix64 random(config.seed ^ 0x52442d524546ULL);
    Polynomial old_coefficients(t + 1);
    for (auto& value : old_coefficients) value = random.next_field();
    std::vector<Fp> old_shares(n);
    for (std::size_t source = 0; source < n; ++source)
        old_shares[source] = evaluate_polynomial(old_coefficients, Fp::from_u64(source + 1));

    Measurement measurement(result, config);
    std::vector<std::vector<Fp>> inputs(n);
    std::vector<bool> input_qualified(n);
    const bool faulty_dealers = scenario == "bad-rg-dealer" || scenario == "combined";
    for (std::size_t dealer = 0; dealer < n; ++dealer) {
        Config instance = config;
        instance.seed = config.seed + 0x9e3779b97f4a7c15ULL * (dealer + 1);
        const auto sharing = reference::run_fdvss2(
            instance, faulty_dealers && dealer < t ? "inconsistent-dealer" : "honest", false
        );
        inputs[dealer] = sharing.shares;
        input_qualified[dealer] = sharing.qualified;
        result.certificates += sharing.certificates;
        result.invalid_certificates += sharing.invalid_certificates;
        result.decoder_failures += sharing.decoder_failures;
        result.communication.p2p_bytes += sharing.communication.p2p_bytes;
        result.communication.broadcast_native_bytes += sharing.communication.broadcast_native_bytes;
        result.communication.broadcast_materialized_bytes += sharing.communication.broadcast_materialized_bytes;
    }
    measurement.stage("rg");
    const std::size_t count = n - t;
    std::vector<Polynomial> random_shares(count, Polynomial(n, Fp::zero()));
    for (std::size_t dealer = 0; dealer < n; ++dealer) {
        const Fp point = Fp::from_u64(dealer + 1);
        Fp power = Fp::one();
        for (std::size_t coefficient = 0; coefficient < count; ++coefficient) {
            for (std::size_t party = 0; party < n; ++party)
                random_shares[coefficient][party] += power * inputs[dealer][party];
            power *= point;
        }
    }
    measurement.stage("rg_vandermonde");

    std::vector<std::vector<Record>> incoming(n, std::vector<Record>(n));
    for (std::size_t source = 0; source < n; ++source) {
        for (std::size_t target = 0; target < n; ++target) {
            const Fp point = Fp::from_u64(target + 1);
            Fp value = old_shares[source];
            Fp power = point;
            for (std::size_t coefficient = 0; coefficient < t; ++coefficient) {
                value += random_shares[coefficient][source] * power;
                power *= point;
            }
            Record record = Polynomial{value};
            if ((scenario == "bad-rd" || scenario == "combined") && source < t)
                (*record)[0] += Fp::from_u64(101 + target + source);
            if (scenario == "missing-rd" && source < t) record.reset();
            if (scenario == "rd-decoder-failure-diagnostic" && source <= t) record.reset();
            const auto wire = encode_record(record);
            result.communication.p2p_bytes += wire.size();
            incoming[target][source] = parse_record(wire);
        }
    }
    bool all_reconstructed = true;
    for (std::size_t target = 0; target < n; ++target) {
        const auto recovered = reconstruct(incoming[target], t, n - t);
        if (!recovered) {
            ++result.decoder_failures;
            all_reconstructed = false;
        } else result.shares[target] = (*recovered)[0];
    }
    result.qualified = all_reconstructed;
    measurement.stage("rd");

    bool inputs_valid = true;
    Polynomial input_secrets(n, Fp::zero());
    for (std::size_t dealer = 0; dealer < n; ++dealer) {
        std::vector<Record> records(n);
        for (std::size_t party = 0; party < n; ++party)
            records[party] = Polynomial{inputs[dealer][party]};
        const auto recovered = reconstruct(records, t, n);
        const bool expected_qualified = !(faulty_dealers && dealer < t);
        SplitMix64 dealer_random(config.seed + 0x9e3779b97f4a7c15ULL * (dealer + 1));
        (void)dealer_random.next_u64();
        const Fp expected_secret = expected_qualified ? dealer_random.next_field() : Fp::zero();
        inputs_valid &= recovered && (*recovered)[0] == expected_secret &&
                        input_qualified[dealer] == expected_qualified;
        if (recovered) input_secrets[dealer] = (*recovered)[0];
    }
    Polynomial random_secrets(count, Fp::zero());
    for (std::size_t dealer = 0; dealer < n; ++dealer) {
        Fp power = Fp::one();
        for (std::size_t coefficient = 0; coefficient < count; ++coefficient) {
            random_secrets[coefficient] += power * input_secrets[dealer];
            power *= Fp::from_u64(dealer + 1);
        }
    }
    bool matches = true;
    for (std::size_t target = 0; target < n; ++target) {
        Fp expected = old_coefficients[0];
        Fp power = Fp::from_u64(target + 1);
        for (std::size_t coefficient = 0; coefficient < t; ++coefficient) {
            expected += random_secrets[coefficient] * power;
            power *= Fp::from_u64(target + 1);
        }
        matches &= result.shares[target] == expected;
    }
    const bool failure_diagnostic = scenario == "rd-decoder-failure-diagnostic";
    result.verified = inputs_valid && (failure_diagnostic
        ? !all_reconstructed && std::all_of(result.shares.begin(), result.shares.end(),
                                            [](Fp value) { return value.is_zero(); })
        : all_reconstructed && matches);
    for (auto& sample : result.stages) sample.verified = result.verified;
    return result;
}

}  // namespace fdvss::reference
