#include "fdvss/reference.hpp"
#include "fdvss/digest.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace fdvss::reference {
namespace {

using Bytes = std::vector<std::uint8_t>;
using Salt = std::array<std::uint64_t, 2>;
using Table = std::vector<Digest256>;

// Fixed little-endian integers, explicit counts, and one-byte optional tags.
// Parsers reject extra bytes, invalid dimensions, and noncanonical fields.
struct Writer {
    Bytes bytes;
    void byte(std::uint8_t value) { bytes.push_back(value); }
    void integer(std::uint64_t value) {
        for (std::size_t k = 0; k < 8; ++k) byte(static_cast<std::uint8_t>(value >> (8 * k)));
    }
    void field(Fp value) { integer(value.value()); }
    void raw(const Bytes& value) { bytes.insert(bytes.end(), value.begin(), value.end()); }
    void frame(const Bytes& value) { integer(value.size()); raw(value); }
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
    Fp field() {
        const auto value = integer();
        if (value >= Fp::modulus) valid = false;
        return Fp::from_u64(value);
    }
    Bytes frame() {
        const auto size = integer();
        if (!valid || size > bytes.size() - offset) { valid = false; return {}; }
        Bytes value(bytes.begin() + offset, bytes.begin() + offset + static_cast<std::size_t>(size));
        offset += static_cast<std::size_t>(size);
        return value;
    }
    bool finished() const { return valid && offset == bytes.size(); }
};

struct Slice {
    Polynomial polynomial;
    Salt salt{};
};
struct Row {
    Polynomial coefficients;  // [X degree][Z degree]
    std::vector<Salt> salts;
};
struct Pair {
    std::optional<Table> table;
    std::vector<std::size_t> senders;
};
struct WitnessRecord {
    std::size_t index = 0;
    Bytes slice;
};
using Certificate = std::vector<WitnessRecord>;
struct PublicMessage {
    Pair pair;
    Bytes certificate;
};

Bytes bottom() { return Bytes{0}; }

Bytes encode_table(const std::optional<Table>& table) {
    Writer writer;
    writer.byte(table ? 1 : 0);
    if (table) {
        writer.integer(table->size());
        for (const auto& digest : *table) {
            writer.bytes.insert(writer.bytes.end(), digest.begin(), digest.end());
        }
    }
    return std::move(writer.bytes);
}

std::optional<Table> parse_table(const Bytes& bytes, std::size_t n) {
    Reader reader{bytes};
    if (reader.byte() != 1 || reader.integer() != n * n) return std::nullopt;
    Table table(n * n);
    for (auto& digest : table) for (auto& value : digest) value = reader.byte();
    return reader.finished() ? std::optional<Table>(std::move(table)) : std::nullopt;
}

Bytes encode_row(const Row& row) {
    Writer writer;
    writer.byte(1);
    writer.integer(row.coefficients.size());
    for (const auto value : row.coefficients) writer.field(value);
    writer.integer(row.salts.size());
    for (const auto& salt : row.salts) for (const auto word : salt) writer.integer(word);
    return std::move(writer.bytes);
}

std::optional<Row> parse_row(const Bytes& bytes, std::size_t n, std::size_t d) {
    Reader reader{bytes};
    if (reader.byte() != 1 || reader.integer() != d * d) return std::nullopt;
    Row row;
    row.coefficients.resize(d * d);
    for (auto& value : row.coefficients) value = reader.field();
    if (reader.integer() != n) return std::nullopt;
    row.salts.resize(n);
    for (auto& salt : row.salts) for (auto& word : salt) word = reader.integer();
    return reader.finished() ? std::optional<Row>(std::move(row)) : std::nullopt;
}

Bytes encode_slice(const Slice& slice) {
    Writer writer;
    writer.byte(1);
    writer.integer(slice.polynomial.size());
    for (const auto value : slice.polynomial) writer.field(value);
    for (const auto word : slice.salt) writer.integer(word);
    return std::move(writer.bytes);
}

std::optional<Slice> parse_slice(const Bytes& bytes, std::size_t d) {
    Reader reader{bytes};
    if (reader.byte() != 1 || reader.integer() != d) return std::nullopt;
    Slice slice;
    slice.polynomial.resize(d);
    for (auto& value : slice.polynomial) value = reader.field();
    for (auto& word : slice.salt) word = reader.integer();
    return reader.finished() ? std::optional<Slice>(std::move(slice)) : std::nullopt;
}

Bytes encode_pair(const Pair& pair) {
    Writer writer;
    writer.frame(encode_table(pair.table));
    writer.integer(pair.senders.size());
    for (const auto index : pair.senders) writer.integer(index);
    return std::move(writer.bytes);
}

std::optional<Pair> parse_pair(const Bytes& bytes, std::size_t n) {
    Reader reader{bytes};
    const auto table_bytes = reader.frame();
    const auto count = reader.integer();
    if (!reader.valid || count > n) return std::nullopt;
    Pair pair{parse_table(table_bytes, n), {}};
    if (!pair.table && table_bytes != bottom()) return std::nullopt;
    for (std::size_t k = 0; k < count; ++k) {
        const auto index = reader.integer();
        if (index >= n || (!pair.senders.empty() && index <= pair.senders.back())) return std::nullopt;
        pair.senders.push_back(static_cast<std::size_t>(index));
    }
    if (!pair.table && !pair.senders.empty()) return std::nullopt;
    return reader.finished() ? std::optional<Pair>(std::move(pair)) : std::nullopt;
}

Bytes encode_certificate(const Certificate& certificate) {
    Writer writer;
    writer.integer(certificate.size());
    for (const auto& record : certificate) {
        writer.integer(record.index);
        writer.frame(record.slice);
    }
    return std::move(writer.bytes);
}

std::optional<Certificate> parse_certificate(const Bytes& bytes, std::size_t n) {
    Reader reader{bytes};
    const auto count = reader.integer();
    if (!reader.valid || count > n) return std::nullopt;
    Certificate certificate;
    for (std::size_t k = 0; k < count; ++k) {
        const auto index = reader.integer();
        const auto slice = reader.frame();
        if (!reader.valid || index >= n) return std::nullopt;
        certificate.push_back({static_cast<std::size_t>(index), slice});
    }
    return reader.finished() ? std::optional<Certificate>(std::move(certificate)) : std::nullopt;
}

Bytes encode_public(const PublicMessage& message) {
    Writer writer;
    writer.frame(encode_pair(message.pair));
    writer.frame(message.certificate);
    return std::move(writer.bytes);
}

std::optional<PublicMessage> parse_public(const Bytes& bytes, std::size_t n) {
    Reader reader{bytes};
    const auto pair_bytes = reader.frame();
    const auto certificate_bytes = reader.frame();
    const auto pair = parse_pair(pair_bytes, n);
    if (!reader.finished() || !pair) return std::nullopt;
    return PublicMessage{*pair, certificate_bytes};
}

Bytes encode_scalar(Fp value) {
    Writer writer;
    writer.byte(1);
    writer.field(value);
    return std::move(writer.bytes);
}

Record parse_scalar(const Bytes& bytes) {
    Reader reader{bytes};
    if (reader.byte() != 1) return std::nullopt;
    const auto value = reader.field();
    return reader.finished() ? Record(Polynomial{value}) : std::nullopt;
}

Digest256 digest_slice(std::uint64_t sid, std::size_t i, std::size_t j, const Slice& slice) {
    static constexpr char domain[] = "FDVSS-2/slice/v1";
    Sha256 hash;
    hash.update(domain, sizeof(domain) - 1);
    sha_update_u64(hash, sid);
    sha_update_u64(hash, i);
    sha_update_u64(hash, j);
    sha_update_u64(hash, slice.salt[0]);
    sha_update_u64(hash, slice.salt[1]);
    sha_update_u64(hash, slice.polynomial.size());
    for (const auto value : slice.polynomial) sha_update_u64(hash, value.value());
    return hash.finish();
}

Polynomial row_at(const Row& row, Fp point, std::size_t d) {
    Polynomial result(d, Fp::zero());
    for (std::size_t z = 0; z < d; ++z) {
        for (std::size_t x = d; x-- > 0;) {
            result[z] = result[z] * point + row.coefficients[x * d + z];
        }
    }
    return result;
}

void point_to_point(Result& result, const Bytes& bytes) {
    result.communication.p2p_bytes += bytes.size();
}
void broadcast(Result& result, const Bytes& bytes, std::size_t recipients) {
    result.communication.broadcast_native_bytes += bytes.size();
    result.communication.broadcast_materialized_bytes += bytes.size() * recipients;
}

std::optional<Pair> recover_table(const std::vector<Bytes>& broadcasts, std::size_t n, std::size_t t) {
    for (std::size_t sender = 0; sender < n; ++sender) {
        const auto table = parse_table(broadcasts[sender], n);
        if (!table) continue;
        Pair pair{table, {}};
        for (std::size_t other = 0; other < n; ++other) {
            if (broadcasts[other] == broadcasts[sender]) pair.senders.push_back(other);
        }
        if (pair.senders.size() >= n - t) return pair;
    }
    return std::nullopt;
}

std::optional<Pair> recover_pair(const std::vector<Bytes>& broadcasts, std::size_t n, std::size_t t) {
    std::vector<std::optional<PublicMessage>> parsed;
    for (const auto& bytes : broadcasts) parsed.push_back(parse_public(bytes, n));
    for (std::size_t sender = 0; sender < n; ++sender) {
        if (!parsed[sender]) continue;
        const auto encoded = encode_pair(parsed[sender]->pair);
        std::size_t count = 0;
        for (const auto& other : parsed) if (other && encode_pair(other->pair) == encoded) ++count;
        if (count >= n - t) return parsed[sender]->pair;
    }
    return std::nullopt;
}

bool certificate_valid(
    const Certificate& certificate, const Pair& pair, std::size_t j,
    std::size_t n, std::size_t t, std::uint64_t sid
) {
    if (!pair.table || certificate.size() != t + 2) return false;
    std::vector<Record> values(n);
    for (const auto& record : certificate) {
        if (record.index >= n || values[record.index] ||
            !std::binary_search(pair.senders.begin(), pair.senders.end(), record.index)) return false;
        const auto slice = parse_slice(record.slice, t + 1);
        if (!slice || digest_slice(sid, record.index, j, *slice) != (*pair.table)[record.index * n + j]) {
            return false;
        }
        values[record.index] = slice->polynomial;
    }
    return !interpolate(values, t).has_value();
}

void overwrite_noncanonical_field(Bytes& bytes, std::size_t offset) {
    if (offset + 8 > bytes.size()) throw std::logic_error("fault offset exceeds message");
    for (std::size_t k = 0; k < 8; ++k) bytes[offset + k] = static_cast<std::uint8_t>(Fp::modulus >> (8 * k));
}

}  // namespace

std::vector<std::string> fdvss2_scenarios() {
    return {"honest", "invalid-slice", "missing-slice", "malformed-slice", "replayed-slice",
            "invalid-dealer-row", "missing-dealer-row", "malformed-dealer-row", "malformed-dealer-table", "table-no-quorum",
            "inconsistent-dealer", "forged-certificate", "duplicate-certificate",
            "malformed-certificate", "consistent-certificate", "excluded-sender-certificate", "bad-pair", "corrupt-z",
            "missing-z", "malformed-z", "insufficient-records-diagnostic"};
}

Result run_fdvss2(const Config& config, const std::string& scenario) {
    const auto scenarios = fdvss2_scenarios();
    if (std::find(scenarios.begin(), scenarios.end(), scenario) == scenarios.end()) {
        throw std::invalid_argument("unknown FDVSS-2 reference scenario: " + scenario);
    }
    const std::size_t n = config.n, t = config.t, d = t + 1;
    if (n == 0 || t >= n || t > (n - 1) / 3 || n > 256) {
        throw std::invalid_argument("reference execution requires 1 <= n <= 256 and n > 3t");
    }
    Result result;
    result.protocol = "fdvss2";
    result.scenario = scenario;
    result.shares.resize(n, Fp::zero());
    SplitMix64 random(config.seed);
    const std::uint64_t sid = random.next_u64();
    Polynomial coefficients(d * d * d);
    for (auto& value : coefficients) value = random.next_field();
    const Fp secret = coefficients[0];
    const auto points = evaluation_points(n);

    // Dealer: generate private rows and authenticate exactly the sent slices.
    std::vector<Row> dealer_rows(n);
    for (std::size_t i = 0; i < n; ++i) {
        auto& row = dealer_rows[i];
        row.coefficients.resize(d * d, Fp::zero());
        for (std::size_t x = 0; x < d; ++x) for (std::size_t z = 0; z < d; ++z) {
            auto& value = row.coefficients[x * d + z];
            for (std::size_t w = d; w-- > 0;) value = value * points[i] + coefficients[(w * d + x) * d + z];
        }
        row.salts.resize(n);
        for (auto& salt : row.salts) for (auto& word : salt) word = random.next_u64();
    }
    if (scenario == "inconsistent-dealer" && n > d) {
        // Every private row still has degree at most t in X and Z. The extra
        // row disagrees with the unique W interpolant through the first t+1.
        dealer_rows[d].coefficients[0] += Fp::one();
    }
    Table dealer_table(n * n);
    for (std::size_t i = 0; i < n; ++i) for (std::size_t j = 0; j < n; ++j) {
        dealer_table[i * n + j] = digest_slice(sid, i, j, {row_at(dealer_rows[i], points[j], d), dealer_rows[i].salts[j]});
    }
    Bytes dealer_table_wire = encode_table(dealer_table);
    if (scenario == "malformed-dealer-table") dealer_table_wire.pop_back();
    broadcast(result, dealer_table_wire, n);
    std::vector<Bytes> dealer_private(n);
    for (std::size_t i = 0; i < n; ++i) {
        dealer_private[i] = encode_row(dealer_rows[i]);
        if (scenario == "invalid-dealer-row" && i < t) {
            auto altered_row = dealer_rows[i];
            altered_row.coefficients[0] += Fp::one();
            dealer_private[i] = encode_row(altered_row);
        }
        if (scenario == "malformed-dealer-row" && i < t) overwrite_noncanonical_field(dealer_private[i], 9);
        if ((scenario == "missing-dealer-row" && i < t) ||
            (scenario == "table-no-quorum" && i < t + 1)) {
            dealer_private[i] = bottom();
        }
        point_to_point(result, dealer_private[i]);
    }

    // Com1: a full-row check controls both its broadcast and private messages.
    std::vector<Bytes> com1_broadcast(n);
    std::vector<std::vector<Bytes>> slices(n, std::vector<Bytes>(n));  // [recipient j][sender i]
    for (std::size_t i = 0; i < n; ++i) {
        const auto table = parse_table(dealer_table_wire, n);
        const auto row = parse_row(dealer_private[i], n, d);
        bool valid = table && row;
        if (valid) for (std::size_t j = 0; j < n; ++j) {
            const Slice slice{row_at(*row, points[j], d), row->salts[j]};
            if (digest_slice(sid, i, j, slice) != (*table)[i * n + j]) valid = false;
        }
        if (!valid) ++result.complaints;
        com1_broadcast[i] = valid ? encode_table(table) : bottom();
        if (scenario == "excluded-sender-certificate" && i < t) com1_broadcast[i] = bottom();
        broadcast(result, com1_broadcast[i], n);
        for (std::size_t j = 0; j < n; ++j) {
            slices[j][i] = valid ? encode_slice({row_at(*row, points[j], d), row->salts[j]}) : bottom();
            if (valid && i < t) {
                if (scenario == "missing-slice") slices[j][i] = bottom();
                if (scenario == "malformed-slice") overwrite_noncanonical_field(slices[j][i], 9);
                if (scenario == "invalid-slice") {
                    auto slice = *parse_slice(slices[j][i], d);
                    slice.polynomial[0] += Fp::one();
                    slices[j][i] = encode_slice(slice);
                }
                if (scenario == "replayed-slice" && j != 0) {
                    slices[j][i] = encode_slice({row_at(*row, points[0], d), row->salts[0]});
                }
            }
            if (scenario == "insufficient-records-diagnostic" && j == 0 && i >= t) slices[j][i] = bottom();
            point_to_point(result, slices[j][i]);
        }
    }

    // Com2: retain original wire openings so a witness cannot accidentally
    // contain reconstructed or normalized values instead of authenticated ones.
    std::vector<Bytes> com2_broadcast(n);
    std::vector<std::vector<Bytes>> final_messages(n, std::vector<Bytes>(n));  // [v][j]
    for (std::size_t j = 0; j < n; ++j) {
        const auto recovered = recover_table(com1_broadcast, n, t);
        Pair pair = recovered.value_or(Pair{});
        Certificate certificate;
        std::optional<Decoded> q;
        if (pair.table) {
            std::vector<Record> retained(n);
            std::vector<std::size_t> basis;
            for (std::size_t i = 0; i < n; ++i) {
                const auto slice = parse_slice(slices[j][i], d);
                if (std::binary_search(pair.senders.begin(), pair.senders.end(), i) && slice &&
                    digest_slice(sid, i, j, *slice) == (*pair.table)[i * n + j]) {
                    retained[i] = slice->polynomial;
                    if (basis.size() < d) basis.push_back(i);
                } else {
                    ++result.complaints;
                }
            }
            if (basis.size() == d) {
                std::vector<Record> basis_values(n);
                for (const auto i : basis) basis_values[i] = retained[i];
                q = interpolate(basis_values, t);
                if (!q) throw std::logic_error("valid interpolation basis unexpectedly failed");
                for (std::size_t i = 0; i < n; ++i) if (retained[i] && evaluate(*q, points[i]) != *retained[i]) {
                    for (const auto index : basis) certificate.push_back({index, slices[j][index]});
                    certificate.push_back({i, slices[j][i]});
                    q.reset();
                    break;
                }
            } else {
                ++result.decoder_failures;
            }
        } else {
            ++result.decoder_failures;
        }

        Bytes certificate_wire = encode_certificate(certificate);
        if (j == 0 && t != 0 && (scenario == "forged-certificate" || scenario == "duplicate-certificate" ||
                scenario == "malformed-certificate" || scenario == "consistent-certificate" ||
                scenario == "excluded-sender-certificate")) {
            Certificate fake;
            for (std::size_t i = 0; i < std::min(n, t + 2); ++i) fake.push_back({i, slices[j][i]});
            if (scenario == "forged-certificate") {
                auto slice = *parse_slice(fake.back().slice, d);
                slice.polynomial[0] += Fp::one();
                fake.back().slice = encode_slice(slice);
            }
            if (scenario == "duplicate-certificate") fake.back() = fake.front();
            certificate_wire = encode_certificate(fake);
            if (scenario == "malformed-certificate") certificate_wire.pop_back();
        }
        const auto parsed_certificate = parse_certificate(certificate_wire, n);
        if (!parsed_certificate || !parsed_certificate->empty()) ++result.certificates;
        if (scenario == "bad-pair" && j < t) pair.senders.clear();
        com2_broadcast[j] = encode_public({pair, certificate_wire});
        broadcast(result, com2_broadcast[j], n);
        for (std::size_t v = 0; v < n; ++v) {
            Fp value = q ? evaluate_polynomial(evaluate(*q, Fp::zero()), points[v]) : Fp::zero();
            if (scenario == "corrupt-z" && j < t) value += points[v];
            final_messages[v][j] = encode_scalar(value);
            if (scenario == "missing-z" && j < t) final_messages[v][j] = bottom();
            if (scenario == "malformed-z" && j < t) overwrite_noncanonical_field(final_messages[v][j], 1);
            point_to_point(result, final_messages[v][j]);
        }
    }

    // Com3: every final recipient recovers the public pair, validates all
    // witnesses, and independently robustly decodes its received z messages.
    std::vector<bool> qualified(n, true);
    for (std::size_t v = 0; v < n; ++v) {
        const auto pair = recover_pair(com2_broadcast, n, t);
        if (!pair || !pair->table || pair->senders.size() < n - t) {
            qualified[v] = false;
            continue;
        }
        for (std::size_t j = 0; j < n; ++j) {
            const auto public_message = parse_public(com2_broadcast[j], n);
            if (!public_message) continue;
            const auto certificate = parse_certificate(public_message->certificate, n);
            if (certificate && certificate->empty()) continue;
            if (certificate && certificate_valid(*certificate, *pair, j, n, t, sid)) {
                qualified[v] = false;
            } else if (v == 0) {
                ++result.invalid_certificates;
            }
        }
        if (!qualified[v]) continue;
        std::vector<Record> values(n);
        for (std::size_t j = 0; j < n; ++j) values[j] = parse_scalar(final_messages[v][j]);
        const auto output = reconstruct(values, t, n - t);
        if (output && output->size() == 1) result.shares[v] = (*output)[0];
        else ++result.decoder_failures;
    }
    result.qualified = qualified[0];
    result.verified = std::all_of(qualified.begin(), qualified.end(), [&](bool status) { return status == result.qualified; });
    for (std::size_t v = 0; v < n; ++v) {
        Polynomial expected_polynomial(coefficients.begin(), coefficients.begin() + d);
        const auto expected = result.qualified ? evaluate_polynomial(expected_polynomial, points[v]) : Fp::zero();
        result.verified = result.verified && result.shares[v] == expected;
    }
    // Check the secret separately from the protocol's qualification decisions.
    if (result.qualified) {
        std::vector<Record> outputs(n);
        for (std::size_t v = 0; v < n; ++v) outputs[v] = Polynomial{result.shares[v]};
        const auto reconstructed = reconstruct(outputs, t, n - t);
        result.verified = result.verified && reconstructed && (*reconstructed)[0] == secret;
    }
    return result;
}

}  // namespace fdvss::reference
