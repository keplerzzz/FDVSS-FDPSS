#include "fdvss/reference.hpp"
#include "fdvss/measurement.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace fdvss::reference {
namespace {

using Indices = std::vector<std::size_t>;
using Complaints = std::vector<Indices>;
using Bytes = std::vector<std::uint8_t>;

struct Writer {
    Bytes bytes;
    void byte(std::uint8_t value) { bytes.push_back(value); }
    void integer(std::uint64_t value) {
        for (std::size_t k = 0; k < 8; ++k) byte(static_cast<std::uint8_t>(value >> (8 * k)));
    }
    void record(const Record& record) {
        byte(record ? 1 : 0);
        if (!record) return;
        integer(record->size());
        for (const auto value : *record) integer(value.value());
    }
    void indices(const Indices& indices) {
        integer(indices.size());
        for (const auto index : indices) integer(index);
    }
    void optional_indices(const std::optional<Indices>& value) {
        byte(value ? 1 : 0);
        if (value) indices(*value);
    }
    void complaints(const std::optional<Complaints>& value) {
        byte(value ? 1 : 0);
        if (!value) return;
        integer(value->size());
        for (const auto& indices : *value) this->indices(indices);
    }
};

struct Reader {
    const Bytes& bytes;
    std::size_t offset = 0;
    bool valid = true;
    std::uint8_t byte() {
        if (offset == bytes.size()) { valid = false; return 0; }
        return bytes[offset++];
    }
    std::uint64_t integer() {
        std::uint64_t value = 0;
        for (std::size_t k = 0; k < 8; ++k) value |= static_cast<std::uint64_t>(byte()) << (8 * k);
        return value;
    }
    bool present() {
        const auto tag = byte();
        if (tag > 1) valid = false;
        return tag == 1;
    }
    Record record() {
        if (!present()) return std::nullopt;
        const auto count = integer();
        if (!valid || count > (bytes.size() - offset) / 8) { valid = false; return std::nullopt; }
        Polynomial polynomial(static_cast<std::size_t>(count));
        for (auto& value : polynomial) {
            const auto encoded = integer();
            if (encoded >= Fp::modulus) valid = false;
            value = Fp::from_u64(encoded);
        }
        return polynomial;
    }
    Indices indices() {
        const auto count = integer();
        if (!valid || count > (bytes.size() - offset) / 8) { valid = false; return {}; }
        Indices result(static_cast<std::size_t>(count));
        for (auto& index : result) index = static_cast<std::size_t>(integer());
        return result;
    }
    std::optional<Indices> optional_indices() {
        return present() ? std::optional<Indices>(indices()) : std::nullopt;
    }
    std::optional<Complaints> complaints() {
        if (!present()) return std::nullopt;
        const auto count = integer();
        if (!valid || count > (bytes.size() - offset) / 8) { valid = false; return std::nullopt; }
        Complaints value(static_cast<std::size_t>(count));
        for (auto& indices : value) indices = this->indices();
        return value;
    }
    bool finished() const { return valid && offset == bytes.size(); }
};

void broadcast(Result& result, const Bytes& bytes, std::size_t n) {
    result.communication.broadcast_native_bytes += bytes.size();
    result.communication.broadcast_materialized_bytes += bytes.size() * n;
}

Record send_record(Result& result, const Record& record) {
    Writer writer;
    writer.record(record);
    result.communication.p2p_bytes += writer.bytes.size();
    Reader reader{writer.bytes};
    auto received = reader.record();
    return reader.finished() ? received : std::nullopt;
}

Indices send_complaint(Result& result, const Indices& indices, std::size_t n) {
    Writer writer;
    writer.indices(indices);
    broadcast(result, writer.bytes, n);
    Reader reader{writer.bytes};
    auto received = reader.indices();
    if (!reader.finished()) throw std::runtime_error("invalid complaint encoding");
    return received;
}

bool valid_indices(const Indices& indices, std::size_t n) {
    for (std::size_t index = 0; index < indices.size(); ++index) {
        if (indices[index] >= n || (index != 0 && indices[index - 1] >= indices[index])) {
            return false;
        }
    }
    return true;
}

Indices all_indices(std::size_t n) {
    Indices indices(n);
    std::iota(indices.begin(), indices.end(), 0);
    return indices;
}

bool contains(const Indices& indices, std::size_t index) {
    return std::binary_search(indices.begin(), indices.end(), index);
}

template <class T>
std::optional<T> majority(const std::vector<std::optional<T>>& messages, std::size_t threshold) {
    for (const auto& message : messages) {
        if (!message) continue;
        const std::size_t count = std::count(messages.begin(), messages.end(), message);
        if (count >= threshold) return message;
    }
    return std::nullopt;
}

Polynomial eval_axis(const Polynomial& coefficients, std::size_t width, Fp point) {
    return evaluate(Decoded{coefficients, {}, width}, point);
}

Polynomial constant_term(const Polynomial& coefficients, std::size_t width) {
    return Polynomial(coefficients.begin(), coefficients.begin() + width);
}

Record valid_record(const Record& record, std::size_t width) {
    return record && record->size() == width ? record : std::nullopt;
}

std::optional<Decoded> decode_records(
    std::vector<Record> records, std::size_t width, std::size_t t,
    std::size_t threshold, Result& result
) {
    for (auto& record : records) record = valid_record(record, width);
    auto polynomial = decode(records, t, threshold);
    if (!polynomial) ++result.decoder_failures;
    return polynomial;
}

Record reconstruct_records(
    const std::vector<Record>& records, std::size_t width, std::size_t t,
    std::size_t threshold, Result& result
) {
    const auto polynomial = decode_records(records, width, t, threshold, result);
    return polynomial ? Record(evaluate(*polynomial, Fp::zero())) : std::nullopt;
}

std::vector<Record> shamir_share(const Record& secret, std::size_t n, std::size_t t, SplitMix64& rng) {
    std::vector<Record> shares(n);
    if (!secret) return shares;
    const std::size_t width = secret->size();
    Polynomial polynomial((t + 1) * width);
    std::copy(secret->begin(), secret->end(), polynomial.begin());
    for (std::size_t coefficient = width; coefficient < polynomial.size(); ++coefficient) {
        polynomial[coefficient] = rng.next_field();
    }
    for (std::size_t recipient = 0; recipient < n; ++recipient) {
        shares[recipient] = eval_axis(polynomial, width, Fp::from_u64(recipient + 1));
    }
    return shares;
}

struct Public3 {
    std::optional<Complaints> complaints;
    bool r_success = false;
    Record complete_vector;
    std::vector<Record> column_vectors;
};

struct Forward4 {
    std::optional<bool> qualified;
    std::optional<Indices> supporters;
    std::optional<Indices> unhappy;
    std::vector<Record> repair_shares;  // [i][k], delivered by this l.
};

Public3 send_public3(Result& result, const Public3& message, std::size_t n) {
    Writer writer;
    writer.complaints(message.complaints);
    writer.byte(message.r_success ? 1 : 0);
    writer.record(message.complete_vector);
    writer.integer(message.column_vectors.size());
    for (const auto& entries : message.column_vectors) {
        writer.byte(entries ? 1 : 0);
        writer.record(entries);
    }
    broadcast(result, writer.bytes, n);
    Reader reader{writer.bytes};
    Public3 received;
    received.complaints = reader.complaints();
    received.r_success = reader.present();
    received.complete_vector = reader.record();
    if (reader.integer() != n) throw std::runtime_error("invalid public column count");
    received.column_vectors.resize(n);
    for (auto& entries : received.column_vectors) {
        const bool successful = reader.present();
        entries = reader.record();
        if (!successful) entries = std::nullopt;
    }
    if (!reader.finished()) throw std::runtime_error("invalid public message encoding");
    return received;
}

Forward4 send_forward4(Result& result, const Forward4& message, std::size_t n) {
    Writer writer;
    writer.byte(message.qualified ? 1 : 0);
    if (message.qualified) writer.byte(*message.qualified ? 1 : 0);
    if (message.qualified.value_or(false)) {
        writer.optional_indices(message.supporters);
        writer.optional_indices(message.unhappy);
        if (message.unhappy) {
            for (const auto i : *message.unhappy) {
                for (std::size_t k = 0; k < n; ++k) writer.record(message.repair_shares[i * n + k]);
            }
        }
    }
    broadcast(result, writer.bytes, n);
    Reader reader{writer.bytes};
    Forward4 received;
    received.repair_shares.resize(n * n);
    if (reader.present()) received.qualified = reader.present();
    if (received.qualified.value_or(false)) {
        received.supporters = reader.optional_indices();
        received.unhappy = reader.optional_indices();
        if (received.unhappy) {
            for (const auto i : *received.unhappy) {
                if (i >= n) throw std::runtime_error("invalid repair index");
                for (std::size_t k = 0; k < n; ++k) received.repair_shares[i * n + k] = reader.record();
            }
        }
    }
    if (!reader.finished()) throw std::runtime_error("invalid forwarding encoding");
    return received;
}

struct Faults {
    bool row = false;
    bool malformed_row = false;
    bool all_malformed = false;
    bool column = false;
    bool committee3 = false;
    bool committee3_values = false;
    bool committee4 = false;
    bool inconsistent_dealer = false;
    bool repair_failure_diagnostic = false;
    bool status_failure_diagnostic = false;
};

Faults faults_for(const std::string& scenario) {
    Faults faults;
    if (scenario == "honest") return faults;
    if (scenario == "bad-row") faults.row = true;
    else if (scenario == "malformed-row") faults.malformed_row = true;
    else if (scenario == "all-malformed-rows") faults.all_malformed = true;
    else if (scenario == "bad-com2") faults.column = true;
    else if (scenario == "bad-com3") faults.committee3 = true;
    else if (scenario == "bad-com3-values") {
        faults.row = true;
        faults.committee3_values = true;
    }
    else if (scenario == "bad-com4") faults.committee4 = true;
    else if (scenario == "combined") {
        faults.row = faults.column = faults.committee3 = faults.committee4 = true;
    } else if (scenario == "inconsistent-dealer" || scenario == "decoder-failure") {
        faults.inconsistent_dealer = true;
    } else if (scenario == "repair-decoder-failure-diagnostic") {
        faults.row = true;
        faults.repair_failure_diagnostic = true;
    } else if (scenario == "status-quorum-failure-diagnostic") {
        faults.status_failure_diagnostic = true;
    } else {
        throw std::invalid_argument("unknown FDVSS-1 reference scenario: " + scenario);
    }
    return faults;
}

}  // namespace

std::vector<std::string> fdvss1_scenarios() {
    return {"honest", "bad-row", "malformed-row", "all-malformed-rows", "bad-com2",
            "bad-com3", "bad-com3-values", "bad-com4", "combined", "inconsistent-dealer", "decoder-failure",
            "repair-decoder-failure-diagnostic", "status-quorum-failure-diagnostic"};
}

Result run_fdvss1(const Config& config, const std::string& scenario) {
    const std::size_t n = config.n;
    const std::size_t t = config.t;
    if (n == 0 || t >= n || t > (n - 1) / 3) {
        throw std::invalid_argument("FDVSS-1 reference execution requires n > 3t");
    }
    const Faults faults = faults_for(scenario);
    if (t == 0 && scenario != "honest") {
        throw std::invalid_argument("FDVSS-1 fault scenarios require t >= 1");
    }
    const std::size_t d = t + 1;
    const std::size_t threshold = n - t;
    const auto points = evaluation_points(n);
    SplitMix64 rng(config.seed);
    Result result;
    result.protocol = "fdvss1";
    result.scenario = scenario;
    result.shares.resize(n, Fp::zero());
    Measurement measurement(result, config);

    Polynomial dealer_polynomial(d * d * d * d);
    for (auto& coefficient : dealer_polynomial) coefficient = rng.next_field();
    const bool expected_qualified = !faults.inconsistent_dealer && !faults.repair_failure_diagnostic
        && !faults.status_failure_diagnostic;
    const auto finish = [&]() {
        measurement.stage("com5");
        const Polynomial expected_secret_z(dealer_polynomial.begin(), dealer_polynomial.begin() + d);
        std::vector<Fp> expected_shares(n);
        for (std::size_t v = 0; v < n; ++v) {
            expected_shares[v] = evaluate_polynomial(expected_secret_z, points[v]);
        }
        if (faults.all_malformed || faults.inconsistent_dealer || faults.repair_failure_diagnostic
            || faults.status_failure_diagnostic) {
            std::fill(expected_shares.begin(), expected_shares.end(), Fp::zero());
        }
        result.verified = result.qualified == expected_qualified && result.shares == expected_shares;
        return result;
    };

    std::vector<Record> rows(n);
    for (std::size_t i = 0; i < n; ++i) {
        rows[i] = eval_axis(dealer_polynomial, d * d * d, points[i]);
        if (faults.inconsistent_dealer) {
            (*rows[i])[0] += points[i].pow(t + 1);
        }
        if (faults.malformed_row && i == 0) rows[i] = Polynomial(d * d * d - 1);
        if (faults.all_malformed) rows[i] = std::nullopt;
        rows[i] = send_record(result, rows[i]);
    }
    measurement.stage("dealer");

    std::vector<Record> a(n * n);  // [i][j], polynomial in Y,Z.
    for (std::size_t i = 0; i < n; ++i) {
        Polynomial row = valid_record(rows[i], d * d * d).value_or(Polynomial(d * d * d));
        if (faults.row && i < t) row[0] += Fp::from_u64(i + 1);
        for (std::size_t j = 0; j < n; ++j) {
            a[i * n + j] = send_record(result, eval_axis(row, d * d, points[j]));
        }
    }
    std::vector<Record>().swap(rows);
    measurement.stage("com1");

    Complaints complaints(n);
    std::vector<Record> raw(n * n * n);  // [i][j][k], polynomial in Z.
    const auto raw_index = [n](std::size_t i, std::size_t j, std::size_t k) {
        return (i * n + j) * n + k;
    };
    for (std::size_t j = 0; j < n; ++j) {
        std::vector<Record> column(n);
        for (std::size_t i = 0; i < n; ++i) column[i] = a[i * n + j];
        const auto q = decode_records(column, d * d, t, threshold, result);
        if (!q) complaints[j] = all_indices(n);
        else {
            for (std::size_t i = 0; i < n; ++i) {
                if (!column[i] || *column[i] != evaluate(*q, points[i])) complaints[j].push_back(i);
            }
        }
        if (faults.column && j < t) complaints[j] = {n};  // Malformed broadcast.
        complaints[j] = send_complaint(result, complaints[j], n);
        for (std::size_t k = 0; k < n; ++k) {
            for (std::size_t i = 0; i < n; ++i) {
                Record value;
                if (const auto entry = valid_record(a[i * n + j], d * d)) {
                    value = eval_axis(*entry, d, points[k]);
                }
                if (faults.column && j < t) {
                    if (j % 2 == 0) value = std::nullopt;
                    else if (value) (*value)[0] += Fp::one();
                }
                raw[raw_index(i, j, k)] = send_record(result, value);
            }
        }
    }
    std::vector<Record>().swap(a);
    measurement.stage("com2");

    for (auto& indices : complaints) {
        if (!valid_indices(indices, n)) indices = all_indices(n);
        result.complaints += indices.size();
    }
    std::vector<std::pair<std::size_t, std::size_t>> edges;
    std::vector<std::vector<std::size_t>> edge_positions(n);
    for (std::size_t j = 0; j < n; ++j) {
        for (const std::size_t i : complaints[j]) {
            edge_positions[j].push_back(edges.size());
            edges.emplace_back(i, j);
        }
    }

    std::vector<Public3> public3(n);
    std::vector<Record> h0(n * n * n), h1(n * n * n);  // [i][k][l].
    const auto h_index = [n](std::size_t i, std::size_t k, std::size_t l) {
        return (i * n + k) * n + l;
    };
    for (std::size_t k = 0; k < n; ++k) {
        std::vector<std::optional<Decoded>> qhat(n), u(n);
        for (std::size_t j = 0; j < n; ++j) {
            std::vector<Record> column(n);
            for (std::size_t i = 0; i < n; ++i) column[i] = raw[raw_index(i, j, k)];
            qhat[j] = decode_records(column, d, t, threshold, result);
        }
        for (std::size_t i = 0; i < n; ++i) {
            std::vector<Record> row(n);
            for (std::size_t j = 0; j < n; ++j) row[j] = raw[raw_index(i, j, k)];
            u[i] = decode_records(row, d, t, threshold, result);
        }
        std::vector<Record> u_coefficients(n);
        for (std::size_t i = 0; i < n; ++i) {
            if (u[i]) u_coefficients[i] = u[i]->coefficients;
        }
        const auto r = decode_records(u_coefficients, d * d, t, threshold, result);
        auto& outgoing = public3[k];
        outgoing.complaints = complaints;
        outgoing.r_success = r.has_value();
        outgoing.column_vectors.resize(n);
        if (r) {
            Polynomial complete;
            complete.reserve(edges.size() * d);
            for (const auto& [i, j] : edges) {
                const auto at_i = evaluate(*r, points[i]);
                const auto at_ij = eval_axis(at_i, d, points[j]);
                complete.insert(complete.end(), at_ij.begin(), at_ij.end());
            }
            outgoing.complete_vector = std::move(complete);
        }
        for (std::size_t j = 0; j < n; ++j) {
            if (!qhat[j]) continue;
            Polynomial entries;
            for (const std::size_t i : complaints[j]) {
                const auto entry = evaluate(*qhat[j], points[i]);
                entries.insert(entries.end(), entry.begin(), entry.end());
            }
            outgoing.column_vectors[j] = std::move(entries);
        }
        if (faults.committee3 && k < t) {
            outgoing.r_success = false;
            outgoing.complete_vector = std::nullopt;
            for (auto& entries : outgoing.column_vectors) entries = std::nullopt;
            outgoing.complaints = std::nullopt;
        }
        if (faults.committee3_values && k < t) {
            if (outgoing.complete_vector && !outgoing.complete_vector->empty()) {
                (*outgoing.complete_vector)[0] += Fp::one();
            }
            for (auto& entries : outgoing.column_vectors) {
                if (entries && !entries->empty()) (*entries)[0] += Fp::one();
            }
        }
        outgoing = send_public3(result, outgoing, n);
        for (std::size_t i = 0; i < n; ++i) {
            Record secret0, secret1;
            if (r) secret0 = constant_term(evaluate(*r, points[i]), d);
            if (u[i]) secret1 = constant_term(u[i]->coefficients, d);
            auto shares0 = shamir_share(secret0, n, t, rng);
            auto shares1 = shamir_share(secret1, n, t, rng);
            for (std::size_t l = 0; l < n; ++l) {
                if (faults.committee3 && k < t) {
                    shares0[l] = std::nullopt;
                    shares1[l] = std::nullopt;
                }
                if (faults.committee3_values && k < t) {
                    if (shares0[l]) (*shares0[l])[0] += Fp::one();
                    if (shares1[l]) (*shares1[l])[0] += Fp::one();
                }
                h0[h_index(i, k, l)] = send_record(result, shares0[l]);
                h1[h_index(i, k, l)] = send_record(result, shares1[l]);
            }
        }
    }
    std::vector<Record>().swap(raw);
    measurement.stage("com3");

    bool qualified = true;
    std::vector<std::optional<Complaints>> complaint_echoes(n);
    for (std::size_t k = 0; k < n; ++k) {
        const auto& candidate = public3[k].complaints;
        if (candidate && candidate->size() == n && std::all_of(candidate->begin(), candidate->end(),
            [n](const auto& indices) { return valid_indices(indices, n); })) {
            complaint_echoes[k] = candidate;
        }
    }
    const auto recovered_complaints = majority(complaint_echoes, threshold);
    if (!recovered_complaints) {
        ++result.decoder_failures;
        qualified = false;
    }
    std::optional<Decoded> p;
    Indices supporters, happy, unhappy;
    if (qualified) {
        complaints = *recovered_complaints;
        edges.clear();
        for (auto& positions : edge_positions) positions.clear();
        for (std::size_t j = 0; j < n; ++j) {
            for (const std::size_t i : complaints[j]) {
                edge_positions[j].push_back(edges.size());
                edges.emplace_back(i, j);
            }
        }
        std::vector<Record> vectors(n);
        for (std::size_t k = 0; k < n; ++k) {
            if (public3[k].r_success) vectors[k] = public3[k].complete_vector;
        }
        p = decode_records(vectors, edges.size() * d, t, threshold, result);
        qualified = p.has_value();
        if (p) supporters = p->supporters;  // Keep all complete supporters.
    }
    if (qualified) {
        Indices good;
        for (std::size_t j = 0; j < n; ++j) {
            std::vector<Record> vectors(n);
            for (std::size_t k = 0; k < n; ++k) vectors[k] = public3[k].column_vectors[j];
            const auto c = decode_records(vectors, complaints[j].size() * d, t, threshold, result);
            if (!c || complaints[j].size() > t) continue;
            Polynomial restriction;
            for (std::size_t degree = 0; degree < d; ++degree) {
                for (const std::size_t position : edge_positions[j]) {
                    const auto begin = p->coefficients.begin() + degree * p->width + position * d;
                    restriction.insert(restriction.end(), begin, begin + d);
                }
            }
            if (restriction == c->coefficients) good.push_back(j);
        }
        if (good.size() < 2 * t + 1) qualified = false;
        else {
            for (std::size_t i = 0; i < n; ++i) {
                std::size_t agreements = 0;
                for (const std::size_t j : good) {
                    if (!contains(complaints[j], i)) ++agreements;
                }
                (agreements >= 2 * t + 1 ? happy : unhappy).push_back(i);
            }
        }
    }
    result.supporters = supporters;
    result.happy = happy;
    result.unhappy = unhappy;

    std::vector<Forward4> forwards(n);
    std::vector<Fp> z(qualified ? n * n * n * n : 0);  // [i][k][l][v].
    std::vector<std::uint8_t> z_present(z.size(), 0);
    const auto z_index = [n](std::size_t i, std::size_t k, std::size_t l, std::size_t v) {
        return ((i * n + k) * n + l) * n + v;
    };
    for (std::size_t l = 0; l < n; ++l) {
        auto& outgoing = forwards[l];
        outgoing.qualified = qualified;
        outgoing.repair_shares.resize(n * n);
        if (qualified) {
            outgoing.supporters = supporters;
            outgoing.unhappy = unhappy;
            for (const std::size_t i : unhappy) {
                for (std::size_t k = 0; k < n; ++k) {
                    outgoing.repair_shares[i * n + k] = h0[h_index(i, k, l)];
                }
            }
        }
        if (faults.committee4 && l < t) {
            outgoing.qualified = !qualified;
            outgoing.supporters = std::nullopt;
            outgoing.unhappy = std::nullopt;
            std::fill(outgoing.repair_shares.begin(), outgoing.repair_shares.end(), std::nullopt);
        }
        // Diagnostic cases omit t+1 senders, exceeding the fault budget.
        if (faults.repair_failure_diagnostic && l <= t) {
            std::fill(outgoing.repair_shares.begin(), outgoing.repair_shares.end(), std::nullopt);
        }
        if (faults.status_failure_diagnostic && l <= t) {
            outgoing.qualified = std::nullopt;
            outgoing.supporters = std::nullopt;
            outgoing.unhappy = std::nullopt;
        }
        outgoing = send_forward4(result, outgoing, n);
        if (!qualified) continue;  // Honest roles forward only status.
        for (std::size_t v = 0; v < n; ++v) {
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t k = 0; k < n; ++k) {
                    Record scalar;
                    if (const auto share = valid_record(h1[h_index(i, k, l)], d)) {
                        scalar = Polynomial{evaluate_polynomial(*share, points[v])};
                    }
                    if (faults.committee4 && l < t) {
                        if (l % 2 == 0) scalar = std::nullopt;
                        else if (scalar) (*scalar)[0] += Fp::one();
                    }
                    const auto received = send_record(result, scalar);
                    const auto position = z_index(i, k, l, v);
                    if (received) {
                        z[position] = (*received)[0];
                        z_present[position] = 1;
                    }
                }
            }
        }
    }
    std::vector<Record>().swap(h0);
    std::vector<Record>().swap(h1);
    measurement.stage("com4");

    std::vector<std::optional<bool>> statuses(n);
    for (std::size_t l = 0; l < n; ++l) statuses[l] = forwards[l].qualified;
    const auto status = majority(statuses, threshold);
    if (!status) ++result.decoder_failures;
    if (!status || !*status) return finish();
    using Selection = std::pair<Indices, Indices>;
    std::vector<std::optional<Selection>> selections(n);
    for (std::size_t l = 0; l < n; ++l) {
        const auto& message = forwards[l];
        if (message.qualified.value_or(false) && message.supporters && message.unhappy
            && valid_indices(*message.supporters, n) && valid_indices(*message.unhappy, n)
            && message.supporters->size() >= threshold) {
            selections[l] = std::make_pair(*message.supporters, *message.unhappy);
        }
    }
    const auto selection = majority(selections, threshold);
    if (!selection) {
        ++result.decoder_failures;
        return finish();
    }
    supporters = selection->first;
    unhappy = selection->second;
    happy.clear();
    for (std::size_t i = 0; i < n; ++i) if (!contains(unhappy, i)) happy.push_back(i);
    result.supporters = supporters;
    result.happy = happy;
    result.unhappy = unhappy;
    std::optional<Decoded> repaired;
    if (!unhappy.empty()) {
        std::vector<Record> repair_vectors(n);
        for (std::size_t k = 0; k < n; ++k) {
            Polynomial complete;
            bool complete_valid = contains(supporters, k);
            for (const std::size_t i : unhappy) {
                std::vector<Record> shares(n);
                for (std::size_t l = 0; l < n; ++l) shares[l] = forwards[l].repair_shares[i * n + k];
                const auto secret = reconstruct_records(shares, d, t, threshold, result);
                if (!secret) complete_valid = false;
                else complete.insert(complete.end(), secret->begin(), secret->end());
            }
            if (complete_valid) repair_vectors[k] = std::move(complete);
        }
        repaired = decode_records(repair_vectors, unhappy.size() * d, t, threshold, result);
        if (!repaired) return finish();
        result.repaired_rows = unhappy.size();
    }

    result.qualified = true;
    const Polynomial repaired_at_zero = repaired ? evaluate(*repaired, Fp::zero()) : Polynomial{};
    for (std::size_t v = 0; v < n; ++v) {
        std::vector<Record> row_values(n);
        for (const std::size_t i : happy) {
            std::vector<Record> k_values(n);
            for (std::size_t k = 0; k < n; ++k) {
                std::vector<Record> l_values(n);
                for (std::size_t l = 0; l < n; ++l) {
                    const auto position = z_index(i, k, l, v);
                    if (z_present[position]) l_values[l] = Polynomial{z[position]};
                }
                k_values[k] = reconstruct_records(l_values, 1, t, threshold, result);
            }
            row_values[i] = reconstruct_records(k_values, 1, t, threshold, result);
        }
        for (std::size_t position = 0; position < unhappy.size(); ++position) {
            const auto begin = repaired_at_zero.begin() + position * d;
            const Polynomial secret_z(begin, begin + d);
            row_values[unhappy[position]] = Polynomial{evaluate_polynomial(secret_z, points[v])};
        }
        const auto share = reconstruct_records(row_values, 1, t, threshold, result);
        result.shares[v] = share ? (*share)[0] : Fp::zero();
    }
    return finish();
}

}  // namespace fdvss::reference
