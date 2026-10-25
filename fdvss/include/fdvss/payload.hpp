#pragma once

#include "fdvss/core.hpp"
#include "fdvss/digest.hpp"

#include <limits>
#include <stdexcept>

namespace fdvss {
namespace payload_detail {

using Bytes = std::vector<std::uint8_t>;
using Salt = std::array<std::uint64_t, 2>;

inline std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
    if (b != 0 && a > std::numeric_limits<std::uint64_t>::max() / b)
        throw std::overflow_error("payload size overflow");
    return a * b;
}

struct Writer {
    Bytes bytes;
    void byte(std::uint8_t value) { bytes.push_back(value); }
    void integer(std::uint64_t value) {
        for (std::size_t k = 0; k < sizeof(value); ++k)
            byte(static_cast<std::uint8_t>(value >> (8 * k)));
    }
    void frame(const Bytes& value) {
        integer(value.size());
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void record(std::size_t count) {
        byte(1);
        integer(count);
        for (std::size_t k = 0; k < count; ++k) integer(0);
    }
    void indices(std::size_t count) {
        integer(count);
        for (std::size_t k = 0; k < count; ++k) integer(k);
    }
};

inline Communication private_messages(const Bytes& bytes, std::uint64_t count) {
    Communication communication;
    communication.p2p_bytes = multiply(bytes.size(), count);
    return communication;
}

inline void broadcasts(Communication& communication, const Bytes& bytes,
                       std::uint64_t count, std::uint64_t recipients) {
    communication.broadcast_native_bytes = multiply(bytes.size(), count);
    communication.broadcast_materialized_bytes =
        multiply(communication.broadcast_native_bytes, recipients);
}

inline Communication scale(Communication communication, std::uint64_t count) {
    communication.p2p_bytes = multiply(communication.p2p_bytes, count);
    communication.broadcast_native_bytes = multiply(communication.broadcast_native_bytes, count);
    communication.broadcast_materialized_bytes = multiply(communication.broadcast_materialized_bytes, count);
    return communication;
}

inline std::vector<Communication> fdvss1(std::size_t n, std::size_t t) {
    const auto d = t + 1;
    const auto n2 = multiply(n, n);
    const auto n3 = multiply(n2, n);
    const auto n4 = multiply(n3, n);
    Writer row, bivariate, univariate, scalar, complaint, public3, forward4;
    row.record(multiply(multiply(d, d), d));
    bivariate.record(multiply(d, d));
    univariate.record(d);
    scalar.record(1);
    complaint.indices(0);
    public3.byte(1);
    public3.integer(n);
    for (std::size_t j = 0; j < n; ++j) public3.indices(0);
    public3.byte(1);
    public3.record(0);
    public3.integer(n);
    for (std::size_t j = 0; j < n; ++j) {
        public3.byte(1);
        public3.record(0);
    }
    forward4.byte(1);
    forward4.byte(1);
    forward4.byte(1);
    forward4.indices(n);
    forward4.byte(1);
    forward4.indices(0);
    std::vector<Communication> stages(6);
    stages[0] = private_messages(row.bytes, n);
    stages[1] = private_messages(bivariate.bytes, n2);
    stages[2] = private_messages(univariate.bytes, n3);
    broadcasts(stages[2], complaint.bytes, n, n);
    stages[3] = private_messages(univariate.bytes, multiply(2, n3));
    broadcasts(stages[3], public3.bytes, n, n);
    stages[4] = private_messages(scalar.bytes, n4);
    broadcasts(stages[4], forward4.bytes, n, n);
    return stages;
}

inline std::vector<Communication> fdvss2(std::size_t n, std::size_t t) {
    const auto d = t + 1;
    const auto n2 = multiply(n, n);
    Writer table, row, slice, pair, certificate, public2, scalar;
    table.byte(1);
    table.integer(n2);
    table.bytes.resize(table.bytes.size() + multiply(n2, sizeof(Digest128)), 0);
    row.byte(1);
    row.integer(multiply(d, d));
    for (std::uint64_t k = 0; k < multiply(d, d); ++k) row.integer(0);
    row.integer(n);
    row.bytes.resize(row.bytes.size() + multiply(n, sizeof(Salt)), 0);
    slice.record(d);
    slice.bytes.resize(slice.bytes.size() + sizeof(Salt), 0);
    pair.frame(table.bytes);
    pair.indices(n);
    certificate.integer(0);
    public2.frame(pair.bytes);
    public2.frame(certificate.bytes);
    scalar.byte(1);
    scalar.integer(0);
    std::vector<Communication> stages(4);
    stages[0] = private_messages(row.bytes, n);
    broadcasts(stages[0], table.bytes, 1, n);
    stages[1] = private_messages(slice.bytes, n2);
    broadcasts(stages[1], table.bytes, n, n);
    stages[2] = private_messages(scalar.bytes, n2);
    broadcasts(stages[2], public2.bytes, n, n);
    return stages;
}

}  // namespace payload_detail

inline std::vector<Communication> honest_payloads(
    const std::string& protocol, std::size_t n, std::size_t t
) {
    if (n == 0 || t > (n - 1) / 3)
        throw std::invalid_argument("payload accounting requires n > 3t");
    if (protocol == "fdvss1") return payload_detail::fdvss1(n, t);
    if (protocol == "fdvss2") return payload_detail::fdvss2(n, t);
    if (protocol == "fdpss2") {
        std::vector<Communication> stages(6);
        const auto sharing = payload_detail::fdvss2(n, t);
        for (std::size_t k = 0; k < sharing.size(); ++k)
            stages[k] = payload_detail::scale(sharing[k], n);
        payload_detail::Writer scalar;
        scalar.record(1);
        stages[5] = payload_detail::private_messages(
            scalar.bytes, payload_detail::multiply(n, n)
        );
        return stages;
    }
    throw std::invalid_argument("unknown payload protocol");
}

}  // namespace fdvss
