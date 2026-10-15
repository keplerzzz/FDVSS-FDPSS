#include "fdvss/core.hpp"
#include "fdvss/digest.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace fdvss {
namespace {

using Clock = std::chrono::steady_clock;

std::size_t resolved_threads(std::size_t requested, std::size_t work_items) {
    const std::size_t hardware = std::max<unsigned>(1U, std::thread::hardware_concurrency());
    const std::size_t wanted = requested == 0 ? hardware : requested;
    return std::max<std::size_t>(1, std::min(wanted, std::max<std::size_t>(1, work_items)));
}

template <class Function>
void parallel_for(std::size_t count, std::size_t requested_threads, Function&& function) {
    if (count == 0) return;
    const std::size_t threads = resolved_threads(requested_threads, count);
    if (threads == 1) {
        for (std::size_t i = 0; i < count; ++i) function(i);
        return;
    }

    std::vector<std::thread> workers;
    workers.reserve(threads - 1);
    std::exception_ptr failure;
    std::mutex failure_mutex;
    auto run_range = [&](std::size_t worker) {
        const std::size_t begin = count * worker / threads;
        const std::size_t end = count * (worker + 1) / threads;
        try {
            for (std::size_t i = begin; i < end; ++i) function(i);
        } catch (...) {
            std::lock_guard lock(failure_mutex);
            if (!failure) failure = std::current_exception();
        }
    };
    for (std::size_t worker = 1; worker < threads; ++worker) {
        workers.emplace_back(run_range, worker);
    }
    run_range(0);
    for (auto& worker : workers) worker.join();
    if (failure) std::rethrow_exception(failure);
}

std::uint64_t mix_seed(std::uint64_t seed, std::size_t stream) {
    SplitMix64 mixer(seed ^ (0x9e37'79b9'7f4a'7c15ULL * (stream + 1)));
    return mixer.next_u64();
}

void fill_random(std::vector<Fp>& values, std::uint64_t seed, std::size_t threads) {
    const std::size_t worker_count = resolved_threads(threads, values.size());
    parallel_for(worker_count, worker_count, [&](std::size_t worker) {
        SplitMix64 random(mix_seed(seed, worker));
        const std::size_t begin = values.size() * worker / worker_count;
        const std::size_t end = values.size() * (worker + 1) / worker_count;
        for (std::size_t i = begin; i < end; ++i) values[i] = random.next_field();
    });
}

std::uint64_t sampled_checksum(std::span<const Fp> values) {
    if (values.empty()) return 0;
    const std::size_t samples = std::min<std::size_t>(4096, values.size());
    std::uint64_t hash = 0xcbf2'9ce4'8422'2325ULL;
    for (std::size_t i = 0; i < samples; ++i) {
        const std::size_t index = i * values.size() / samples;
        hash ^= values[index].value();
        hash *= 0x0000'0100'0000'01b3ULL;
    }
    hash ^= static_cast<std::uint64_t>(values.size());
    return hash;
}

std::uint64_t saturated_bytes(std::initializer_list<std::size_t> element_counts) {
    unsigned __int128 total = 0;
    for (const std::size_t count : element_counts) total += count;
    total *= sizeof(Fp);
    if (total > std::numeric_limits<std::uint64_t>::max()) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(total);
}

std::size_t checked_product(std::initializer_list<std::size_t> factors) {
    std::size_t result = 1;
    for (const std::size_t factor : factors) {
        if (factor != 0 && result > std::numeric_limits<std::size_t>::max() / factor) {
            throw std::overflow_error("experiment shape overflows size_t");
        }
        result *= factor;
    }
    return result;
}

template <class Function>
double timed(Function&& function) {
    const auto begin = Clock::now();
    function();
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}

std::vector<Fp> multiply_by_linear(std::span<const Fp> polynomial, Fp root) {
    std::vector<Fp> result(polynomial.size() + 1, Fp::zero());
    for (std::size_t i = 0; i < polynomial.size(); ++i) {
        result[i] -= polynomial[i] * root;
        result[i + 1] += polynomial[i];
    }
    return result;
}

std::vector<Fp> product_from_roots(std::span<const Fp> roots) {
    std::vector<Fp> result{Fp::one()};
    for (const Fp root : roots) result = multiply_by_linear(result, root);
    return result;
}

std::vector<Fp> divide_by_linear(
    std::span<const Fp> polynomial,
    Fp root
) {
    if (polynomial.size() < 2) throw std::invalid_argument("linear division needs degree >= 1");
    const std::size_t degree = polynomial.size() - 1;
    std::vector<Fp> quotient(degree, Fp::zero());
    quotient[degree - 1] = polynomial[degree];
    for (std::size_t coefficient = degree - 1; coefficient > 0; --coefficient) {
        quotient[coefficient - 1] = polynomial[coefficient] + root * quotient[coefficient];
    }
    const Fp remainder = polynomial[0] + root * quotient[0];
    if (!remainder.is_zero()) throw std::logic_error("synthetic division failed");
    return quotient;
}

Digest256 digest_slice(
    std::uint64_t sid,
    std::size_t i,
    std::size_t j,
    const std::array<std::uint64_t, 2>& salt,
    std::span<const Fp> coefficients
) {
    static constexpr char domain[] = "FDVSS-2/slice/v1";
    Sha256 hash;
    hash.update(domain, sizeof(domain) - 1);
    sha_update_u64(hash, sid);
    sha_update_u64(hash, static_cast<std::uint64_t>(i));
    sha_update_u64(hash, static_cast<std::uint64_t>(j));
    sha_update_u64(hash, salt[0]);
    sha_update_u64(hash, salt[1]);
    sha_update_u64(hash, static_cast<std::uint64_t>(coefficients.size()));
    for (const Fp coefficient : coefficients) sha_update_u64(hash, coefficient.value());
    return hash.finish();
}

std::string json_escape(const std::string& input) {
    std::ostringstream output;
    for (const unsigned char byte : input) {
        switch (byte) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (byte < 0x20) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<unsigned>(byte) << std::dec;
                } else {
                    output << static_cast<char>(byte);
                }
        }
    }
    return output.str();
}

void require_config(const Config& config) {
    if (config.n < 3 * config.t + 1) {
        throw std::invalid_argument("FDVSS requires n >= 3t + 1");
    }
    if (config.n == 0 || config.t + 1 > config.n) {
        throw std::invalid_argument("invalid n/t parameters");
    }
}

StageSample base_stage(const Config& config, std::string protocol, std::string mode, std::string stage) {
    StageSample result;
    result.protocol = std::move(protocol);
    result.mode = std::move(mode);
    result.stage = std::move(stage);
    result.n = config.n;
    result.t = config.t;
    result.threads = resolved_threads(config.threads, config.n);
    return result;
}

}  // namespace

InterpolationPlan::InterpolationPlan(std::size_t dimension)
    : InterpolationPlan(evaluation_points(dimension)) {}

InterpolationPlan::InterpolationPlan(std::vector<Fp> points)
    : dimension_(points.size()), points_(std::move(points)) {
    if (dimension_ == 0) throw std::invalid_argument("empty interpolation domain");
    for (std::size_t i = 0; i < dimension_; ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (points_[i] == points_[j]) throw std::invalid_argument("duplicate interpolation point");
        }
    }

    const auto vanishing = product_from_roots(points_);
    std::vector<std::vector<Fp>> quotients(dimension_);
    std::vector<Fp> denominators(dimension_, Fp::zero());
    for (std::size_t point = 0; point < dimension_; ++point) {
        quotients[point] = divide_by_linear(vanishing, points_[point]);
        denominators[point] = evaluate_polynomial(quotients[point], points_[point]);
    }
    const auto inverses = batch_inverse(denominators);
    basis_.assign(dimension_ * dimension_, Fp::zero());
    for (std::size_t coefficient = 0; coefficient < dimension_; ++coefficient) {
        for (std::size_t point = 0; point < dimension_; ++point) {
            basis_[coefficient * dimension_ + point] =
                quotients[point][coefficient] * inverses[point];
        }
    }
    zero_weights_.resize(dimension_);
    for (std::size_t point = 0; point < dimension_; ++point) {
        zero_weights_[point] = basis_[point];
    }
}

std::vector<Fp> InterpolationPlan::interpolate_prefix(
    std::span<const Fp> values,
    std::size_t columns,
    std::size_t threads
) const {
    if (columns == 0 || values.size() != dimension_ * columns) {
        throw std::invalid_argument("invalid interpolation matrix shape");
    }
    std::vector<Fp> result(dimension_ * columns, Fp::zero());
    parallel_for(dimension_, threads, [&](std::size_t coefficient) {
        Fp* output = result.data() + coefficient * columns;
        for (std::size_t point = 0; point < dimension_; ++point) {
            const Fp factor = basis(coefficient, point);
            const Fp* input = values.data() + point * columns;
            for (std::size_t column = 0; column < columns; ++column) {
                output[column] += factor * input[column];
            }
        }
    });
    return result;
}

Fp InterpolationPlan::interpolate_zero(std::span<const Fp> values) const {
    if (values.size() != dimension_) throw std::invalid_argument("invalid scalar interpolation shape");
    Fp result = Fp::zero();
    for (std::size_t i = 0; i < dimension_; ++i) result += zero_weights_[i] * values[i];
    return result;
}

std::vector<Fp> evaluation_points(std::size_t count) {
    if (count >= Fp::modulus) throw std::invalid_argument("too many evaluation points");
    std::vector<Fp> points(count);
    for (std::size_t i = 0; i < count; ++i) points[i] = Fp::from_u64(i + 1);
    return points;
}

Fp evaluate_polynomial(std::span<const Fp> coefficients, Fp point) {
    if (coefficients.empty()) return Fp::zero();
    Fp result = coefficients.back();
    for (std::size_t i = coefficients.size() - 1; i > 0; --i) {
        result *= point;
        result += coefficients[i - 1];
    }
    return result;
}

std::vector<Fp> evaluate_matrix(
    std::span<const Fp> coefficients,
    std::size_t coefficient_count,
    std::size_t columns,
    std::span<const Fp> points,
    std::size_t threads
) {
    if (coefficient_count == 0 || columns == 0 ||
        coefficients.size() != coefficient_count * columns) {
        throw std::invalid_argument("invalid evaluation matrix shape");
    }
    std::vector<Fp> result(checked_product({points.size(), columns}));
    parallel_for(points.size(), threads, [&](std::size_t point_index) {
        Fp* output = result.data() + point_index * columns;
        const Fp point = points[point_index];
        std::copy_n(
            coefficients.data() + (coefficient_count - 1) * columns,
            columns,
            output
        );
        for (std::size_t coefficient = coefficient_count - 1; coefficient > 0; --coefficient) {
            const Fp* input = coefficients.data() + (coefficient - 1) * columns;
            for (std::size_t column = 0; column < columns; ++column) {
                output[column] *= point;
                output[column] += input[column];
            }
        }
    });
    return result;
}

namespace {

StageSample run_fdpss2_dealer(
    const Config& config,
    const std::string& protocol,
    const std::string& mode,
    std::span<const Fp> points,
    std::size_t threads
) {
    using Salt = std::array<std::uint64_t, 2>;
    const std::size_t n = config.n;
    const std::size_t d = config.t + 1;
    const std::uint64_t sid = config.seed ^ 0x2'0101ULL;
    StageSample stage = base_stage(config, protocol, mode, "rg_fdvss2_dealer");
    std::vector<Fp> coefficients;
    std::vector<Fp> rows;
    std::vector<Fp> slices;
    std::vector<Salt> salts;
    std::vector<Digest256> digests;
    stage.milliseconds = timed([&] {
        coefficients.resize(checked_product({d, d, d}));
        fill_random(coefficients, config.seed ^ 0x2'0111ULL, threads);
        coefficients[0] = Fp::from_u64(0x52);
        rows = evaluate_matrix(coefficients, d, d * d, points, threads);
        slices.assign(checked_product({n, n, d}), Fp::zero());
        parallel_for(checked_product({n, n}), threads, [&](std::size_t pair) {
            const std::size_t i = pair / n;
            const std::size_t j = pair % n;
            Fp* output = slices.data() + pair * d;
            const Fp* row = rows.data() + i * d * d;
            std::copy_n(row + (d - 1) * d, d, output);
            for (std::size_t x = d - 1; x > 0; --x) {
                const Fp* input = row + (x - 1) * d;
                for (std::size_t z = 0; z < d; ++z) {
                    output[z] *= points[j];
                    output[z] += input[z];
                }
            }
        });
        salts.resize(checked_product({n, n}));
        parallel_for(salts.size(), threads, [&](std::size_t index) {
            SplitMix64 random(mix_seed(config.seed ^ 0x2'0122ULL, index));
            salts[index] = {random.next_u64(), random.next_u64()};
        });
        digests.resize(checked_product({n, n}));
        parallel_for(digests.size(), threads, [&](std::size_t pair) {
            const std::size_t i = pair / n;
            const std::size_t j = pair % n;
            digests[pair] = digest_slice(
                sid, i, j, salts[pair],
                std::span<const Fp>(slices.data() + pair * d, d)
            );
        });
    });
    stage.peak_live_bytes = saturated_bytes({coefficients.size(), rows.size(), slices.size()}) +
        static_cast<std::uint64_t>(salts.size() * sizeof(Salt) + digests.size() * sizeof(Digest256));
    stage.communication.p2p_bytes = static_cast<std::uint64_t>(rows.size()) * sizeof(Fp) +
        static_cast<std::uint64_t>(salts.size()) * sizeof(Salt);
    stage.communication.broadcast_native_bytes =
        static_cast<std::uint64_t>(digests.size()) * sizeof(Digest256);
    stage.communication.broadcast_materialized_bytes = stage.communication.broadcast_native_bytes * n;
    stage.checksum = sampled_checksum(slices);
    stage.verified = !digests.empty() && digests[0] == digest_slice(
        sid, 0, 0, salts[0], std::span<const Fp>(slices.data(), d)
    );
    return stage;
}

StageSample run_fdpss2_com1_batch(
    const Config& config,
    const std::string& protocol,
    const std::string& mode,
    std::span<const Fp> points,
    std::size_t threads
) {
    using Salt = std::array<std::uint64_t, 2>;
    const std::size_t n = config.n;
    const std::size_t d = config.t + 1;
    const std::size_t columns = checked_product({n, d});
    const std::uint64_t sid = config.seed ^ 0x2'0202ULL;
    StageSample stage = base_stage(config, protocol, mode, "rg_fdvss2_com1_batch_n");

    std::vector<Fp> rows(checked_product({d, columns}));
    fill_random(rows, config.seed ^ 0x2'0212ULL, threads);
    const std::vector<Fp> expected = evaluate_matrix(rows, d, columns, points, threads);
    std::vector<Salt> salts(checked_product({n, n}));
    std::vector<Digest256> digests(checked_product({n, n}));
    parallel_for(salts.size(), threads, [&](std::size_t pair) {
        SplitMix64 random(mix_seed(config.seed ^ 0x2'0222ULL, pair));
        salts[pair] = {random.next_u64(), random.next_u64()};
        const std::size_t instance = pair / n;
        const std::size_t j = pair % n;
        digests[pair] = digest_slice(
            sid ^ instance, 0, j, salts[pair],
            std::span<const Fp>(expected.data() + j * columns + instance * d, d)
        );
    });

    std::vector<Fp> actual;
    std::atomic<bool> valid{true};
    stage.milliseconds = timed([&] {
        actual = evaluate_matrix(rows, d, columns, points, threads);
        parallel_for(checked_product({n, n}), threads, [&](std::size_t pair) {
            const std::size_t instance = pair / n;
            const std::size_t j = pair % n;
            if (digest_slice(
                    sid ^ instance, 0, j, salts[pair],
                    std::span<const Fp>(actual.data() + j * columns + instance * d, d)
                ) != digests[pair]) {
                valid.store(false, std::memory_order_relaxed);
            }
        });
    });
    valid.store(valid.load(std::memory_order_relaxed) && actual == expected, std::memory_order_relaxed);
    stage.peak_live_bytes = saturated_bytes({rows.size(), expected.size(), actual.size()}) +
        static_cast<std::uint64_t>(salts.size() * sizeof(Salt) + digests.size() * sizeof(Digest256));
    stage.communication.p2p_bytes = static_cast<std::uint64_t>(actual.size()) * sizeof(Fp) +
        static_cast<std::uint64_t>(salts.size()) * sizeof(Salt);
    const std::uint64_t table_bytes = static_cast<std::uint64_t>(n) * n * sizeof(Digest256);
    stage.communication.broadcast_native_bytes = static_cast<std::uint64_t>(n) * table_bytes;
    stage.communication.broadcast_materialized_bytes = stage.communication.broadcast_native_bytes * n;
    stage.checksum = sampled_checksum(actual);
    stage.verified = valid.load(std::memory_order_relaxed);
    return stage;
}

StageSample run_fdpss2_com2_batch(
    const Config& config,
    const std::string& protocol,
    const std::string& mode,
    std::span<const Fp> points,
    const InterpolationPlan& plan,
    std::size_t threads
) {
    using Salt = std::array<std::uint64_t, 2>;
    const std::size_t n = config.n;
    const std::size_t d = config.t + 1;
    const std::size_t columns = checked_product({n, d});
    const std::uint64_t sid = config.seed ^ 0x2'0303ULL;
    StageSample stage = base_stage(config, protocol, mode, "rg_fdvss2_com2_batch_n");

    std::vector<Fp> source(checked_product({d, columns}));
    fill_random(source, config.seed ^ 0x2'0313ULL, threads);
    const std::vector<Fp> received = evaluate_matrix(source, d, columns, points, threads);
    std::vector<Salt> salts(checked_product({n, n}));
    std::vector<Digest256> digests(checked_product({n, n}));
    parallel_for(salts.size(), threads, [&](std::size_t pair) {
        SplitMix64 random(mix_seed(config.seed ^ 0x2'0323ULL, pair));
        salts[pair] = {random.next_u64(), random.next_u64()};
        const std::size_t instance = pair / n;
        const std::size_t sender = pair % n;
        digests[pair] = digest_slice(
            sid ^ instance, sender, 0, salts[pair],
            std::span<const Fp>(received.data() + sender * columns + instance * d, d)
        );
    });

    std::vector<Fp> decoded;
    std::vector<Fp> outputs;
    std::atomic<bool> valid{true};
    stage.milliseconds = timed([&] {
        parallel_for(checked_product({n, n}), threads, [&](std::size_t pair) {
            const std::size_t instance = pair / n;
            const std::size_t sender = pair % n;
            if (digest_slice(
                    sid ^ instance, sender, 0, salts[pair],
                    std::span<const Fp>(received.data() + sender * columns + instance * d, d)
                ) != digests[pair]) {
                valid.store(false, std::memory_order_relaxed);
            }
        });
        decoded = plan.interpolate_prefix(
            std::span<const Fp>(received.data(), d * columns), columns, threads
        );
        std::vector<Fp> reencoded = evaluate_matrix(decoded, d, columns, points, threads);
        parallel_for(received.size(), threads, [&](std::size_t index) {
            if (received[index] != reencoded[index]) {
                valid.store(false, std::memory_order_relaxed);
            }
        });

        outputs.assign(checked_product({n, n}), Fp::zero());
        parallel_for(n, threads, [&](std::size_t instance) {
            const Fp* q_at_w_zero = decoded.data() + instance * d;
            for (std::size_t target = 0; target < n; ++target) {
                outputs[instance * n + target] = evaluate_polynomial(
                    std::span<const Fp>(q_at_w_zero, d), points[target]
                );
            }
        });
    });
    valid.store(valid.load(std::memory_order_relaxed) && decoded == source, std::memory_order_relaxed);
    stage.peak_live_bytes = saturated_bytes({source.size(), received.size(), decoded.size(), outputs.size()}) +
        static_cast<std::uint64_t>(salts.size() * sizeof(Salt) + digests.size() * sizeof(Digest256));
    stage.communication.p2p_bytes = static_cast<std::uint64_t>(outputs.size()) * sizeof(Fp);
    const std::uint64_t table_bytes = static_cast<std::uint64_t>(n) * n * sizeof(Digest256);
    stage.communication.broadcast_native_bytes = static_cast<std::uint64_t>(n) *
        (table_bytes + (n + 7) / 8 + sizeof(std::uint32_t));
    stage.communication.broadcast_materialized_bytes = stage.communication.broadcast_native_bytes * n;
    stage.checksum = sampled_checksum(outputs);
    stage.verified = valid.load(std::memory_order_relaxed);
    return stage;
}

StageSample run_fdpss2_com3_batch(
    const Config& config,
    const std::string& protocol,
    const std::string& mode,
    std::span<const Fp> points,
    const InterpolationPlan& plan,
    std::size_t threads
) {
    const std::size_t n = config.n;
    const std::size_t d = config.t + 1;
    StageSample stage = base_stage(config, protocol, mode, "rg_fdvss2_com3_batch_n");
    std::vector<Fp> coefficients(checked_product({d, n})); // [degree][instance]
    fill_random(coefficients, config.seed ^ 0x2'0404ULL, threads);
    const std::vector<Fp> incoming = evaluate_matrix(
        coefficients, d, n, points, threads
    ); // [sender][instance]
    std::vector<Fp> outputs(n, Fp::zero());
    stage.milliseconds = timed([&] {
        parallel_for(n, threads, [&](std::size_t instance) {
            Fp value = Fp::zero();
            for (std::size_t sender = 0; sender < d; ++sender) {
                value += plan.zero_weight(sender) * incoming[sender * n + instance];
            }
            outputs[instance] = value;
        });
    });
    bool valid = true;
    for (std::size_t instance = 0; instance < n; ++instance) {
        valid &= outputs[instance] == coefficients[instance];
    }
    stage.peak_live_bytes = saturated_bytes({coefficients.size(), incoming.size(), outputs.size()});
    stage.checksum = sampled_checksum(outputs);
    stage.verified = valid;
    return stage;
}

std::vector<Fp> solve_linear_system(
    std::vector<Fp> augmented,
    std::size_t rows,
    std::size_t columns
) {
    if (augmented.size() != rows * (columns + 1)) {
        throw std::invalid_argument("invalid linear-system shape");
    }
    std::vector<std::size_t> pivot_rows(columns, rows);
    std::size_t next_row = 0;
    for (std::size_t column = 0; column < columns; ++column) {
        std::size_t pivot = next_row;
        while (pivot < rows && augmented[pivot * (columns + 1) + column].is_zero()) ++pivot;
        if (pivot == rows) throw std::runtime_error("rank-deficient robust reconstruction system");
        if (pivot != next_row) {
            for (std::size_t entry = column; entry <= columns; ++entry) {
                std::swap(
                    augmented[pivot * (columns + 1) + entry],
                    augmented[next_row * (columns + 1) + entry]
                );
            }
        }
        const Fp inverse = augmented[next_row * (columns + 1) + column].inverse();
        for (std::size_t entry = column; entry <= columns; ++entry) {
            augmented[next_row * (columns + 1) + entry] *= inverse;
        }
        for (std::size_t row = 0; row < rows; ++row) {
            if (row == next_row) continue;
            const Fp factor = augmented[row * (columns + 1) + column];
            if (factor.is_zero()) continue;
            for (std::size_t entry = column; entry <= columns; ++entry) {
                augmented[row * (columns + 1) + entry] -=
                    factor * augmented[next_row * (columns + 1) + entry];
            }
        }
        pivot_rows[column] = next_row++;
    }
    for (std::size_t row = next_row; row < rows; ++row) {
        bool zero = true;
        for (std::size_t column = 0; column < columns; ++column) {
            zero &= augmented[row * (columns + 1) + column].is_zero();
        }
        if (zero && !augmented[row * (columns + 1) + columns].is_zero()) {
            throw std::runtime_error("inconsistent robust reconstruction system");
        }
    }
    std::vector<Fp> solution(columns, Fp::zero());
    for (std::size_t column = 0; column < columns; ++column) {
        solution[column] = augmented[pivot_rows[column] * (columns + 1) + columns];
    }
    return solution;
}

Fp robust_reconstruct_zero(
    std::span<const Fp> points,
    std::span<const Fp> values,
    std::size_t degree,
    std::size_t errors
) {
    if (points.size() != values.size() || points.size() < degree + 2 * errors + 1) {
        throw std::invalid_argument("insufficient robust-reconstruction points");
    }
    const std::size_t q_degree = degree + errors;
    const std::size_t q_count = q_degree + 1;
    const std::size_t unknowns = q_count + errors;
    std::vector<Fp> augmented(points.size() * (unknowns + 1), Fp::zero());
    for (std::size_t row = 0; row < points.size(); ++row) {
        Fp power = Fp::one();
        for (std::size_t k = 0; k < q_count; ++k) {
            augmented[row * (unknowns + 1) + k] = power;
            power *= points[row];
        }
        power = Fp::one();
        for (std::size_t k = 0; k < errors; ++k) {
            augmented[row * (unknowns + 1) + q_count + k] =
                Fp::zero() - values[row] * power;
            power *= points[row];
        }
        augmented[row * (unknowns + 1) + unknowns] = values[row] * power;
    }
    const std::vector<Fp> solution = solve_linear_system(
        std::move(augmented), points.size(), unknowns
    );
    std::vector<Fp> remainder(solution.begin(), solution.begin() + q_count);
    std::vector<Fp> locator(errors + 1, Fp::zero());
    for (std::size_t k = 0; k < errors; ++k) locator[k] = solution[q_count + k];
    locator[errors] = Fp::one();
    std::vector<Fp> quotient(degree + 1, Fp::zero());
    for (std::size_t current = q_degree + 1; current-- > errors;) {
        const Fp factor = remainder[current];
        quotient[current - errors] = factor;
        for (std::size_t k = 0; k <= errors; ++k) {
            remainder[current - errors + k] -= factor * locator[k];
        }
    }
    for (std::size_t k = 0; k < errors; ++k) {
        if (!remainder[k].is_zero()) throw std::runtime_error("nonzero decoder remainder");
    }
    std::size_t support = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        support += evaluate_polynomial(quotient, points[i]) == values[i] ? 1 : 0;
    }
    if (support < points.size() - errors) {
        throw std::runtime_error("robust reconstruction lacks n-t support");
    }
    return quotient[0];
}

}  // namespace

RunRecord run_fdpss(
    const Config& raw_config,
    bool inject_redistribution_fault
) {
    Config config = raw_config;
    if (config.t == 0 && config.n > 1) config.t = (config.n - 1) / 3;
    require_config(config);
    const std::size_t n = config.n;
    const std::size_t t = config.t;
    const std::size_t d = t + 1;
    const std::size_t threads = resolved_threads(config.threads, n);
    const std::vector<Fp> points = evaluation_points(n);
    const InterpolationPlan plan(d);
    const std::string protocol = "fdpss2";
    const std::string mode = std::string("representative-node-fdvss2") +
        (inject_redistribution_fault ? "-rd-byzantine" : "-honest");
    RunRecord record{protocol, mode, n, t, threads, config.seed, true, {}};

    auto append = [&](StageSample stage) {
        record.verified &= stage.verified;
        record.stages.push_back(std::move(stage));
    };
    append(run_fdpss2_dealer(config, protocol, mode, points, threads));
    append(run_fdpss2_com1_batch(config, protocol, mode, points, threads));
    append(run_fdpss2_com2_batch(config, protocol, mode, points, plan, threads));
    append(run_fdpss2_com3_batch(config, protocol, mode, points, plan, threads));

    // Randomness-generation output contract: n independently committed inputs become
    // n-t shared random values through the public Vandermonde transform.
    std::vector<Fp> inputs(n);
    std::vector<Fp> input_polynomials(checked_product({d, n}));
    std::vector<Fp> input_shares;
    {
        StageSample stage = base_stage(config, protocol, mode, "rg_output_contract_all_parties");
        stage.milliseconds = timed([&] {
            fill_random(inputs, config.seed ^ 0x3'0101ULL, threads);
            fill_random(input_polynomials, config.seed ^ 0x3'0111ULL, threads);
            std::copy(inputs.begin(), inputs.end(), input_polynomials.begin());
            input_shares = evaluate_matrix(input_polynomials, d, n, points, threads);
        });
        bool valid = true;
        for (std::size_t input = 0; input < n; ++input) {
            Fp recovered = Fp::zero();
            for (std::size_t party = 0; party < d; ++party) {
                recovered += plan.zero_weight(party) * input_shares[party * n + input];
            }
            valid &= recovered == inputs[input];
        }
        stage.peak_live_bytes = saturated_bytes({
            inputs.size(), input_polynomials.size(), input_shares.size()
        });
        stage.checksum = sampled_checksum(input_shares);
        stage.verified = valid;
        append(std::move(stage));
    }

    const std::size_t random_count = n - t;
    std::vector<Fp> random_secrets(random_count, Fp::zero());
    for (std::size_t input = 0; input < n; ++input) {
        Fp power = Fp::one();
        for (std::size_t k = 0; k < random_count; ++k) {
            random_secrets[k] += power * inputs[input];
            power *= points[input];
        }
    }

    {
        StageSample stage = base_stage(config, protocol, mode, "rg_vandermonde_party0");
        std::vector<Fp> party_output(random_count, Fp::zero());
        stage.milliseconds = timed([&] {
            for (std::size_t input = 0; input < n; ++input) {
                Fp power = Fp::one();
                const Fp share = input_shares[input];
                for (std::size_t k = 0; k < random_count; ++k) {
                    party_output[k] += power * share;
                    power *= points[input];
                }
            }
        });
        stage.peak_live_bytes = saturated_bytes({n, party_output.size()});
        stage.checksum = sampled_checksum(party_output);
        stage.verified = !party_output.empty();
        append(std::move(stage));
    }

    std::vector<Fp> random_shares(checked_product({random_count, n}), Fp::zero());
    {
        StageSample stage = base_stage(config, protocol, mode, "rg_vandermonde_all_parties");
        stage.milliseconds = timed([&] {
            parallel_for(n, threads, [&](std::size_t party) {
                for (std::size_t input = 0; input < n; ++input) {
                    Fp power = Fp::one();
                    const Fp share = input_shares[party * n + input];
                    for (std::size_t k = 0; k < random_count; ++k) {
                        random_shares[k * n + party] += power * share;
                        power *= points[input];
                    }
                }
            });
        });
        bool valid = true;
        for (std::size_t k = 0; k < random_count; ++k) {
            valid &= plan.interpolate_zero(
                std::span<const Fp>(random_shares.data() + k * n, d)
            ) == random_secrets[k];
        }
        stage.peak_live_bytes = saturated_bytes({input_shares.size(), random_shares.size()});
        stage.checksum = sampled_checksum(random_shares);
        stage.verified = valid;
        append(std::move(stage));
    }

    // One-round redistribution from Com_k to Com_{k+1}.
    const Fp secret = Fp::from_u64(0x5a17'2026ULL);
    std::vector<Fp> old_polynomial(d);
    fill_random(old_polynomial, config.seed ^ 0x4'0101ULL, threads);
    old_polynomial[0] = secret;
    const std::vector<Fp> old_shares = evaluate_matrix(old_polynomial, d, 1, points, threads);

    std::vector<Fp> new_polynomial(d, Fp::zero());
    new_polynomial[0] = secret;
    for (std::size_t coefficient = 1; coefficient < d; ++coefficient) {
        new_polynomial[coefficient] = random_secrets[coefficient - 1];
    }
    const std::vector<Fp> expected_new_shares =
        evaluate_matrix(new_polynomial, d, 1, points, threads);

    const auto redistribution_value = [&](std::size_t old_party, std::size_t new_party) {
        Fp result = old_shares[old_party];
        Fp power = points[new_party];
        for (std::size_t coefficient = 0; coefficient < t; ++coefficient) {
            result += random_shares[coefficient * n + old_party] * power;
            power *= points[new_party];
        }
        return result;
    };

    // Vary the Byzantine sender set by seed so repeated negative runs exercise
    // different error locations instead of only a fixed prefix.
    std::vector<std::size_t> corrupted_senders(n);
    std::iota(corrupted_senders.begin(), corrupted_senders.end(), 0);
    if (inject_redistribution_fault) {
        SplitMix64 permutation(config.seed ^ 0x4'0202ULL);
        for (std::size_t remaining = n; remaining > 1; --remaining) {
            const std::size_t chosen = static_cast<std::size_t>(
                permutation.next_u64() % remaining
            );
            std::swap(corrupted_senders[remaining - 1], corrupted_senders[chosen]);
        }
        corrupted_senders.resize(t);
    } else {
        corrupted_senders.clear();
    }

    {
        StageSample stage = base_stage(config, protocol, mode, "rd_party0_send_receive");
        std::vector<Fp> outgoing(n, Fp::zero());
        std::vector<Fp> incoming(n, Fp::zero());
        Fp recovered = Fp::zero();
        stage.milliseconds = timed([&] {
            for (std::size_t new_party = 0; new_party < n; ++new_party) {
                outgoing[new_party] = redistribution_value(0, new_party);
            }
            for (std::size_t old_party = 0; old_party < n; ++old_party) {
                incoming[old_party] = redistribution_value(old_party, 0);
            }
            if (inject_redistribution_fault) {
                for (const std::size_t old_party : corrupted_senders) {
                    incoming[old_party] += Fp::from_u64(old_party + 1);
                }
                recovered = robust_reconstruct_zero(points, incoming, t, t);
            } else {
                recovered = plan.interpolate_zero(std::span<const Fp>(incoming.data(), d));
            }
        });
        stage.peak_live_bytes = saturated_bytes({outgoing.size(), incoming.size()});
        stage.communication.p2p_bytes = static_cast<std::uint64_t>(n) * sizeof(Fp);
        stage.checksum = recovered.value() ^ sampled_checksum(outgoing);
        stage.verified = recovered == expected_new_shares[0];
        append(std::move(stage));
    }

    std::vector<Fp> messages(checked_product({n, n}), Fp::zero()); // [new][old]
    std::vector<Fp> new_shares(n, Fp::zero());
    {
        StageSample stage = base_stage(config, protocol, mode, "rd_all_parties");
        std::atomic<bool> valid{true};
        stage.milliseconds = timed([&] {
            parallel_for(n, threads, [&](std::size_t old_party) {
                for (std::size_t new_party = 0; new_party < n; ++new_party) {
                    messages[new_party * n + old_party] =
                        redistribution_value(old_party, new_party);
                }
            });
            if (inject_redistribution_fault) {
                for (const std::size_t old_party : corrupted_senders) {
                    for (std::size_t new_party = 0; new_party < n; ++new_party) {
                        messages[new_party * n + old_party] +=
                            Fp::from_u64(1 + old_party + new_party);
                    }
                }
            }
            parallel_for(n, threads, [&](std::size_t new_party) {
                try {
                    const std::span<const Fp> row(messages.data() + new_party * n, n);
                    new_shares[new_party] = inject_redistribution_fault
                        ? robust_reconstruct_zero(points, row, t, t)
                        : plan.interpolate_zero(row.first(d));
                } catch (...) {
                    valid.store(false, std::memory_order_relaxed);
                }
            });
        });
        valid.store(
            valid.load(std::memory_order_relaxed) && new_shares == expected_new_shares,
            std::memory_order_relaxed
        );
        stage.peak_live_bytes = saturated_bytes({messages.size(), new_shares.size()});
        stage.communication.p2p_bytes = static_cast<std::uint64_t>(messages.size()) * sizeof(Fp);
        stage.checksum = sampled_checksum(new_shares);
        stage.verified = valid.load(std::memory_order_relaxed);
        append(std::move(stage));
    }

    {
        StageSample stage = base_stage(config, protocol, mode, "transition_verify");
        Fp old_secret = Fp::zero();
        Fp new_secret = Fp::zero();
        bool fresh = false;
        stage.milliseconds = timed([&] {
            old_secret = plan.interpolate_zero(std::span<const Fp>(old_shares.data(), d));
            new_secret = plan.interpolate_zero(std::span<const Fp>(new_shares.data(), d));
            fresh = old_polynomial != new_polynomial && old_shares != new_shares;
        });
        stage.peak_live_bytes = saturated_bytes({old_shares.size(), new_shares.size()});
        stage.checksum = old_secret.value() ^ new_secret.value() ^ sampled_checksum(new_shares);
        stage.verified = old_secret == secret && new_secret == secret && fresh;
        append(std::move(stage));
    }

    return record;
}

bool self_test(std::string* failure) {
    try {
        // Goldilocks reduction against the compiler's exact 128-bit remainder.
        SplitMix64 random(0x1234'5678'90ab'cdefULL);
        for (std::size_t i = 0; i < 20000; ++i) {
            const std::uint64_t a = random.next_field().value();
            const std::uint64_t b = random.next_field().value();
            const auto exact = static_cast<std::uint64_t>(
                (static_cast<unsigned __int128>(a) * b) % Fp::modulus
            );
            if ((Fp::from_u64(a) * Fp::from_u64(b)).value() != exact) {
                throw std::runtime_error("Goldilocks multiplication mismatch");
            }
        }

        // SHA-256("abc").
        Sha256 hash;
        static constexpr char abc[] = "abc";
        hash.update(abc, 3);
        const Digest256 digest = hash.finish();
        static constexpr std::array<std::uint8_t, 32> expected = {
            0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
            0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
        };
        if (digest != expected) throw std::runtime_error("SHA-256 test vector mismatch");

        // Matrix evaluation/interpolation round trip.
        const std::size_t d = 4;
        const std::size_t columns = 13;
        std::vector<Fp> coefficients(d * columns);
        fill_random(coefficients, 77, 2);
        const auto points = evaluation_points(8);
        const auto values = evaluate_matrix(coefficients, d, columns, points, 2);
        const InterpolationPlan plan(d);
        const auto recovered = plan.interpolate_prefix(
            std::span<const Fp>(values.data(), d * columns), columns, 2
        );
        if (recovered != coefficients) throw std::runtime_error("interpolation round trip failed");

        // A t+2 authenticated set that cannot lie on one degree-t polynomial.
        const InterpolationPlan line_plan(2);
        std::array<Fp, 2> line_coeff{Fp::from_u64(9), Fp::from_u64(7)};
        const auto cert_points = evaluation_points(3);
        std::array<Fp, 3> cert_values{
            evaluate_polynomial(line_coeff, cert_points[0]),
            evaluate_polynomial(line_coeff, cert_points[1]),
            evaluate_polynomial(line_coeff, cert_points[2]) + Fp::one()
        };
        const Fp intercept = line_plan.interpolate_zero(
            std::span<const Fp>(cert_values.data(), 2)
        );
        std::array<Fp, 2> first_two{cert_values[0], cert_values[1]};
        const auto recovered_line = line_plan.interpolate_prefix(first_two, 1, 1);
        if (intercept != line_coeff[0] ||
            evaluate_polynomial(recovered_line, cert_points[2]) == cert_values[2]) {
            throw std::runtime_error("inconsistency certificate test failed");
        }

        Config small;
        small.n = 4;
        small.t = 1;
        small.threads = 2;
        small.seed = 91;
        if (!run_fdpss(small, false).verified) {
            throw std::runtime_error("FDPSS honest transition failed");
        }
        if (!run_fdpss(small, true).verified) {
            throw std::runtime_error("FDPSS Byzantine redistribution test failed");
        }
        return true;
    } catch (const std::exception& error) {
        if (failure) *failure = error.what();
        return false;
    }
}

void write_csv(const std::filesystem::path& path, const std::vector<RunRecord>& records) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    if (!output) throw std::runtime_error("cannot open CSV output: " + path.string());
    output << "protocol,mode,n,t,threads,seed,run_verified,stage,stage_ms,peak_live_bytes,"
              "p2p_bytes,broadcast_native_bytes,broadcast_materialized_bytes,stage_verified,checksum\n";
    output << std::setprecision(10);
    for (const RunRecord& record : records) {
        for (const StageSample& stage : record.stages) {
            output << record.protocol << ',' << record.mode << ',' << record.n << ',' << record.t << ','
                   << record.threads << ',' << record.seed << ',' << (record.verified ? 1 : 0) << ','
                   << stage.stage << ',' << stage.milliseconds << ',' << stage.peak_live_bytes << ','
                   << stage.communication.p2p_bytes << ',' << stage.communication.broadcast_native_bytes << ','
                   << stage.communication.broadcast_materialized_bytes << ',' << (stage.verified ? 1 : 0)
                   << ',' << stage.checksum << '\n';
        }
    }
}

void write_json(const std::filesystem::path& path, const std::vector<RunRecord>& records) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    if (!output) throw std::runtime_error("cannot open JSON output: " + path.string());
    output << "{\n  \"schema\": \"fdpss-benchmark-v1\",\n  \"system\": \""
           << json_escape(system_fingerprint()) << "\",\n  \"records\": [\n";
    output << std::setprecision(10);
    for (std::size_t r = 0; r < records.size(); ++r) {
        const RunRecord& record = records[r];
        output << "    {\"protocol\":\"" << json_escape(record.protocol)
               << "\",\"mode\":\"" << json_escape(record.mode) << "\",\"n\":" << record.n
               << ",\"t\":" << record.t << ",\"threads\":" << record.threads
               << ",\"seed\":" << record.seed << ",\"verified\":"
               << (record.verified ? "true" : "false") << ",\"stages\":[\n";
        for (std::size_t s = 0; s < record.stages.size(); ++s) {
            const StageSample& stage = record.stages[s];
            output << "      {\"name\":\"" << json_escape(stage.stage) << "\",\"ms\":"
                   << stage.milliseconds << ",\"peak_live_bytes\":" << stage.peak_live_bytes
                   << ",\"p2p_bytes\":" << stage.communication.p2p_bytes
                   << ",\"broadcast_native_bytes\":" << stage.communication.broadcast_native_bytes
                   << ",\"broadcast_materialized_bytes\":" << stage.communication.broadcast_materialized_bytes
                   << ",\"verified\":" << (stage.verified ? "true" : "false")
                   << ",\"checksum\":" << stage.checksum << '}'
                   << (s + 1 == record.stages.size() ? "\n" : ",\n");
        }
        output << "    ]}" << (r + 1 == records.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
}

std::string system_fingerprint() {
    std::ostringstream output;
#if defined(__clang__)
    output << "clang " << __clang_major__ << '.' << __clang_minor__ << '.' << __clang_patchlevel__;
#elif defined(__GNUC__)
    output << "gcc " << __GNUC__ << '.' << __GNUC_MINOR__ << '.' << __GNUC_PATCHLEVEL__;
#else
    output << "unknown-compiler";
#endif
#if defined(__aarch64__) || defined(__arm64__)
    output << "; arm64";
#elif defined(__x86_64__)
    output << "; x86_64";
#endif
    output << "; hw_threads=" << std::thread::hardware_concurrency();
    output << "; field=goldilocks";
    output << "; digest=sha256";
    return output.str();
}

}  // namespace fdvss
