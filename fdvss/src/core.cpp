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

RunRecord run_fdvss1_roles(const Config& raw_config) {
    Config config = raw_config;
    if (config.t == 0 && config.n > 1) config.t = (config.n - 1) / 3;
    require_config(config);
    const std::size_t n = config.n;
    const std::size_t d = config.t + 1;
    const std::size_t threads = resolved_threads(config.threads, n);
    const auto points = evaluation_points(n);
    const InterpolationPlan plan(d);

    RunRecord record{"fdvss1", "fdvss1-honest-role-kernels", n, config.t, threads, config.seed, true, {}};

    // Dealer: sample F(W,X,Y,Z) and produce all F(i,X,Y,Z) rows.
    {
        StageSample stage = base_stage(config, record.protocol, record.mode, "dealer");
        const std::size_t columns = checked_product({d, d, d});
        std::vector<Fp> coefficients;
        std::vector<Fp> rows;
        stage.milliseconds = timed([&] {
            coefficients.resize(checked_product({d, columns}));
            fill_random(coefficients, config.seed ^ 0x101ULL, threads);
            coefficients[0] = Fp::from_u64(42);
            rows = evaluate_matrix(coefficients, d, columns, points, threads);
        });
        stage.peak_live_bytes = saturated_bytes({coefficients.size(), rows.size()});
        stage.communication.p2p_bytes = static_cast<std::uint64_t>(rows.size()) * sizeof(Fp);
        stage.checksum = sampled_checksum(rows);
        stage.verified = !rows.empty() && rows[0] == [&] {
            std::vector<Fp> scalar(d);
            for (std::size_t w = 0; w < d; ++w) scalar[w] = coefficients[w * columns];
            return evaluate_polynomial(scalar, points[0]);
        }();
        record.verified &= stage.verified;
        record.stages.push_back(std::move(stage));
    }

    // Representative Com1 party: evaluate one trivariate row in X.
    {
        StageSample stage = base_stage(config, record.protocol, record.mode, "com1");
        const std::size_t yz = checked_product({d, d});
        std::vector<Fp> row(checked_product({d, yz}));
        fill_random(row, config.seed ^ 0x202ULL, threads);
        std::vector<Fp> slices;
        stage.milliseconds = timed([&] { slices = evaluate_matrix(row, d, yz, points, threads); });
        stage.peak_live_bytes = saturated_bytes({row.size(), slices.size()});
        stage.communication.p2p_bytes = static_cast<std::uint64_t>(slices.size()) * sizeof(Fp);
        stage.checksum = sampled_checksum(slices);
        std::vector<Fp> scalar(d);
        for (std::size_t x = 0; x < d; ++x) scalar[x] = row[x * yz];
        stage.verified = slices[0] == evaluate_polynomial(scalar, points[0]);
        record.verified &= stage.verified;
        record.stages.push_back(std::move(stage));
    }

    // Representative Com2 party: honest vector RS decode and raw Y forwarding.
    {
        StageSample stage = base_stage(config, record.protocol, record.mode, "com2");
        const std::size_t yz = checked_product({d, d});
        std::vector<Fp> source_coefficients(checked_product({d, yz}));
        fill_random(source_coefficients, config.seed ^ 0x303ULL, threads);
        const std::vector<Fp> received = evaluate_matrix(source_coefficients, d, yz, points, threads);
        std::vector<Fp> decoded;
        std::vector<Fp> forwarded;
        bool all_valid = true;
        stage.milliseconds = timed([&] {
            decoded = plan.interpolate_prefix(
                std::span<const Fp>(received.data(), d * yz), yz, threads
            );
            std::vector<Fp> reencoded = evaluate_matrix(decoded, d, yz, points, threads);
            std::atomic<bool> valid{true};
            parallel_for(received.size(), threads, [&](std::size_t index) {
                if (received[index] != reencoded[index]) valid.store(false, std::memory_order_relaxed);
            });
            all_valid = valid.load(std::memory_order_relaxed);
            reencoded.clear();
            reencoded.shrink_to_fit();

            forwarded.assign(checked_product({n, n, d}), Fp::zero());
            parallel_for(n, threads, [&](std::size_t row) {
                const Fp* polynomial = received.data() + row * yz;
                for (std::size_t target = 0; target < n; ++target) {
                    Fp* output = forwarded.data() + (row * n + target) * d;
                    for (std::size_t y = d; y-- > 0;) {
                        const Fp* coefficient = polynomial + y * d;
                        for (std::size_t z = 0; z < d; ++z) {
                            output[z] *= points[target];
                            output[z] += coefficient[z];
                        }
                    }
                }
            });
        });
        stage.peak_live_bytes = saturated_bytes({
            source_coefficients.size(), received.size(), decoded.size(), forwarded.size()
        });
        stage.communication.p2p_bytes = static_cast<std::uint64_t>(forwarded.size()) * sizeof(Fp);
        stage.communication.broadcast_native_bytes = (n + 7) / 8;
        stage.communication.broadcast_materialized_bytes =
            stage.communication.broadcast_native_bytes * n;
        stage.checksum = sampled_checksum(forwarded);
        stage.verified = all_valid && decoded == source_coefficients;
        record.verified &= stage.verified;
        record.stages.push_back(std::move(stage));
    }

    // Representative Com3 party: two axis decodes, R recovery, and polynomial-valued Shamir handoff.
    {
        StageSample stage = base_stage(config, record.protocol, record.mode, "com3_decode_and_reshare");
        const std::size_t xz = checked_product({d, d});
        std::vector<Fp> source(checked_product({d, xz}));
        fill_random(source, config.seed ^ 0x404ULL, threads);
        const std::vector<Fp> w_values = evaluate_matrix(source, d, xz, points, threads);
        std::vector<Fp> table(checked_product({n, n, d}), Fp::zero()); // [i][j][z]
        parallel_for(n, threads, [&](std::size_t i) {
            const Fp* x_coefficients = w_values.data() + i * xz;
            for (std::size_t j = 0; j < n; ++j) {
                Fp* output = table.data() + (i * n + j) * d;
                for (std::size_t x = d; x-- > 0;) {
                    const Fp* coefficient = x_coefficients + x * d;
                    for (std::size_t z = 0; z < d; ++z) {
                        output[z] *= points[j];
                        output[z] += coefficient[z];
                    }
                }
            }
        });

        std::vector<Fp> qhat;
        std::vector<Fp> u;
        std::vector<Fp> recovered;
        std::vector<Fp> shares;
        stage.milliseconds = timed([&] {
            qhat.assign(checked_product({n, d, d}), Fp::zero()); // [j][w][z]
            parallel_for(checked_product({n, d}), threads, [&](std::size_t task) {
                const std::size_t j = task / d;
                const std::size_t w = task % d;
                Fp* output = qhat.data() + (j * d + w) * d;
                for (std::size_t i = 0; i < d; ++i) {
                    const Fp factor = plan.basis(w, i);
                    const Fp* input = table.data() + (i * n + j) * d;
                    for (std::size_t z = 0; z < d; ++z) output[z] += factor * input[z];
                }
            });

            u.assign(checked_product({n, d, d}), Fp::zero()); // [i][x][z]
            parallel_for(checked_product({n, d}), threads, [&](std::size_t task) {
                const std::size_t i = task / d;
                const std::size_t x = task % d;
                Fp* output = u.data() + (i * d + x) * d;
                for (std::size_t j = 0; j < d; ++j) {
                    const Fp factor = plan.basis(x, j);
                    const Fp* input = table.data() + (i * n + j) * d;
                    for (std::size_t z = 0; z < d; ++z) output[z] += factor * input[z];
                }
            });

            recovered.assign(checked_product({d, d, d}), Fp::zero()); // [w][x][z]
            parallel_for(checked_product({d, d}), threads, [&](std::size_t task) {
                const std::size_t w = task / d;
                const std::size_t x = task % d;
                Fp* output = recovered.data() + (w * d + x) * d;
                for (std::size_t i = 0; i < d; ++i) {
                    const Fp factor = plan.basis(w, i);
                    const Fp* input = u.data() + (i * d + x) * d;
                    for (std::size_t z = 0; z < d; ++z) output[z] += factor * input[z];
                }
            });

            std::vector<Fp> r_x0(checked_product({d, d}));
            for (std::size_t w = 0; w < d; ++w) {
                const Fp* source_row = recovered.data() + w * d * d;
                std::copy_n(source_row, d, r_x0.data() + w * d);
            }
            const std::vector<Fp> r_secrets = evaluate_matrix(r_x0, d, d, points, threads);

            const std::size_t secret_columns = checked_product({2, n, d});
            std::vector<Fp> share_coefficients(checked_product({d, secret_columns}));
            fill_random(share_coefficients, config.seed ^ 0x505ULL, threads);
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t z = 0; z < d; ++z) {
                    share_coefficients[i * d + z] = r_secrets[i * d + z];
                    share_coefficients[n * d + i * d + z] = u[(i * d) * d + z];
                }
            }
            shares = evaluate_matrix(share_coefficients, d, secret_columns, points, threads);
        });
        stage.peak_live_bytes = saturated_bytes({
            source.size(), w_values.size(), table.size(), qhat.size(), u.size(), recovered.size(), shares.size()
        });
        stage.communication.p2p_bytes = static_cast<std::uint64_t>(shares.size()) * sizeof(Fp);
        stage.communication.broadcast_native_bytes = static_cast<std::uint64_t>((n + 7) / 8 + 2);
        stage.communication.broadcast_materialized_bytes =
            stage.communication.broadcast_native_bytes * n;
        stage.checksum = sampled_checksum(shares);
        stage.verified = recovered == source;
        record.verified &= stage.verified;
        record.stages.push_back(std::move(stage));
    }

    if (config.include_final_handoff) {
        // Representative Com4 party: evaluate h^1_{ik,l}(Z) for every final receiver v.
        StageSample stage = base_stage(config, record.protocol, record.mode, "com4_forward");
        const std::size_t columns = checked_product({n, n});
        std::vector<Fp> polynomial_shares(checked_product({d, columns}));
        fill_random(polynomial_shares, config.seed ^ 0x606ULL, threads);
        std::vector<Fp> forwarded;
        stage.milliseconds = timed([&] {
            forwarded = evaluate_matrix(polynomial_shares, d, columns, points, threads);
        });
        stage.peak_live_bytes = saturated_bytes({polynomial_shares.size(), forwarded.size()});
        stage.communication.p2p_bytes = static_cast<std::uint64_t>(forwarded.size()) * sizeof(Fp);
        stage.communication.broadcast_native_bytes =
            static_cast<std::uint64_t>(2 * ((n + 7) / 8) + 1);
        stage.communication.broadcast_materialized_bytes =
            stage.communication.broadcast_native_bytes * n;
        stage.checksum = sampled_checksum(forwarded);
        std::vector<Fp> scalar(d);
        for (std::size_t z = 0; z < d; ++z) scalar[z] = polynomial_shares[z * columns];
        stage.verified = forwarded[0] == evaluate_polynomial(scalar, points[0]);
        record.verified &= stage.verified;
        record.stages.push_back(std::move(stage));

        // Representative Com5 party: honest nested reconstruction over l, k, then i.
        StageSample final_stage = base_stage(config, record.protocol, record.mode, "com5_share_generation");
        std::vector<Fp> incoming(checked_product({n, n, n})); // [i][k][l]
        fill_random(incoming, config.seed ^ 0x707ULL, threads);
        Fp final_value = Fp::zero();
        final_stage.milliseconds = timed([&] {
            std::vector<Fp> after_l(checked_product({n, n}), Fp::zero());
            parallel_for(checked_product({n, n}), threads, [&](std::size_t pair) {
                Fp value = Fp::zero();
                const Fp* row = incoming.data() + pair * n;
                for (std::size_t l = 0; l < d; ++l) value += plan.zero_weight(l) * row[l];
                after_l[pair] = value;
            });
            std::vector<Fp> after_k(n, Fp::zero());
            parallel_for(n, threads, [&](std::size_t i) {
                Fp value = Fp::zero();
                for (std::size_t k = 0; k < d; ++k) {
                    value += plan.zero_weight(k) * after_l[i * n + k];
                }
                after_k[i] = value;
            });
            for (std::size_t i = 0; i < d; ++i) final_value += plan.zero_weight(i) * after_k[i];
        });
        final_stage.peak_live_bytes = saturated_bytes({incoming.size(), n * n, n});
        final_stage.checksum = final_value.value();
        final_stage.verified = true;
        record.stages.push_back(std::move(final_stage));
    }

    return record;
}

RunRecord run_fdvss2(const Config& raw_config, bool inject_fault) {
    Config config = raw_config;
    if (config.t == 0 && config.n > 1) config.t = (config.n - 1) / 3;
    require_config(config);
    const std::size_t n = config.n;
    const std::size_t d = config.t + 1;
    const std::size_t threads = resolved_threads(config.threads, n);
    const auto points = evaluation_points(n);
    const InterpolationPlan plan(d);
    const std::uint64_t sid = config.seed ^ 0xfdf5'5200'0000'0002ULL;
    const Fp secret = Fp::from_u64(0x1234'5678'9abc'def0ULL);

    const std::string mode = inject_fault ? "fdvss2-invalid-record" : "fdvss2-honest";
    RunRecord record{"fdvss2", mode, n, config.t, threads, config.seed, true, {}};

    using Salt = std::array<std::uint64_t, 2>;
    std::vector<Fp> coefficients;
    std::vector<Fp> rows;       // [i][x][z]
    std::vector<Fp> slices;     // [i][j][z]
    std::vector<Salt> salts;    // [i][j]
    std::vector<Digest256> digests;

    {
        StageSample stage = base_stage(config, record.protocol, record.mode, "dealer");
        stage.milliseconds = timed([&] {
            coefficients.resize(checked_product({d, d, d}));
            fill_random(coefficients, config.seed ^ 0x811ULL, threads);
            coefficients[0] = secret;
            rows = evaluate_matrix(coefficients, d, d * d, points, threads);

            slices.assign(checked_product({n, n, d}), Fp::zero());
            parallel_for(checked_product({n, n}), threads, [&](std::size_t pair) {
                const std::size_t i = pair / n;
                const std::size_t j = pair % n;
                Fp* output = slices.data() + pair * d;
                const Fp* row = rows.data() + i * d * d;
                for (std::size_t x = d; x-- > 0;) {
                    const Fp* input = row + x * d;
                    for (std::size_t z = 0; z < d; ++z) {
                        output[z] *= points[j];
                        output[z] += input[z];
                    }
                }
            });

            salts.resize(checked_product({n, n}));
            parallel_for(salts.size(), threads, [&](std::size_t index) {
                SplitMix64 random(mix_seed(config.seed ^ 0x822ULL, index));
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
        stage.communication.broadcast_native_bytes = static_cast<std::uint64_t>(digests.size()) * sizeof(Digest256);
        stage.communication.broadcast_materialized_bytes = stage.communication.broadcast_native_bytes * n;
        stage.checksum = sampled_checksum(slices);
        stage.verified = !slices.empty() && !digests.empty();
        record.verified &= stage.verified;
        record.stages.push_back(std::move(stage));
    }

    std::vector<Fp> forwarded_slices(checked_product({n, n, d}), Fp::zero());
    std::vector<std::uint8_t> row_valid(n, 1);

    // Representative Com1 party timing.
    {
        StageSample stage = base_stage(config, record.protocol, record.mode, "com1_party0");
        std::vector<Fp> party_slices;
        bool valid = false;
        stage.milliseconds = timed([&] {
            party_slices = evaluate_matrix(
                std::span<const Fp>(rows.data(), d * d), d, d, points, threads
            );
            valid = true;
            for (std::size_t j = 0; j < n; ++j) {
                const std::size_t pair = j;
                valid &= digest_slice(
                    sid, 0, j, salts[pair],
                    std::span<const Fp>(party_slices.data() + j * d, d)
                ) == digests[pair];
            }
        });
        stage.peak_live_bytes = saturated_bytes({d * d, party_slices.size()}) +
                                static_cast<std::uint64_t>(n * sizeof(Salt));
        const std::uint64_t record_bytes = static_cast<std::uint64_t>(d * sizeof(Fp) + sizeof(Salt));
        stage.communication.p2p_bytes = static_cast<std::uint64_t>(n) * record_bytes;
        stage.communication.broadcast_native_bytes =
            static_cast<std::uint64_t>(digests.size()) * sizeof(Digest256);
        stage.communication.broadcast_materialized_bytes =
            stage.communication.broadcast_native_bytes * n;
        stage.checksum = sampled_checksum(party_slices);
        stage.verified = valid;
        record.verified &= stage.verified;
        record.stages.push_back(std::move(stage));
    }

    {
        StageSample stage = base_stage(config, record.protocol, record.mode, "com1_all_parties");
        stage.milliseconds = timed([&] {
            parallel_for(n, threads, [&](std::size_t i) {
                const Fp* row = rows.data() + i * d * d;
                for (std::size_t j = 0; j < n; ++j) {
                    Fp* output = forwarded_slices.data() + (i * n + j) * d;
                    for (std::size_t x = d; x-- > 0;) {
                        const Fp* input = row + x * d;
                        for (std::size_t z = 0; z < d; ++z) {
                            output[z] *= points[j];
                            output[z] += input[z];
                        }
                    }
                }
                bool valid = true;
                for (std::size_t j = 0; j < n; ++j) {
                    const std::size_t pair = i * n + j;
                    valid &= digest_slice(
                        sid, i, j, salts[pair],
                        std::span<const Fp>(forwarded_slices.data() + pair * d, d)
                    ) == digests[pair];
                }
                row_valid[i] = static_cast<std::uint8_t>(valid);
            });
        });
        if (inject_fault) {
            // A malicious Com1 sender changes one authenticated record after its local check.
            const std::size_t bad_i = n - 1;
            const std::size_t bad_j = 0;
            forwarded_slices[(bad_i * n + bad_j) * d] += Fp::one();
        }
        stage.peak_live_bytes = saturated_bytes({rows.size(), forwarded_slices.size()}) +
                                static_cast<std::uint64_t>(digests.size() * sizeof(Digest256));
        const std::uint64_t record_bytes = static_cast<std::uint64_t>(d * sizeof(Fp) + sizeof(Salt));
        stage.communication.p2p_bytes = static_cast<std::uint64_t>(n) * n * record_bytes;
        stage.communication.broadcast_native_bytes = static_cast<std::uint64_t>(n) * digests.size() * sizeof(Digest256);
        stage.communication.broadcast_materialized_bytes = stage.communication.broadcast_native_bytes * n;
        stage.checksum = std::accumulate(row_valid.begin(), row_valid.end(), std::uint64_t{0});
        stage.verified = std::all_of(row_valid.begin(), row_valid.end(), [](std::uint8_t value) { return value != 0; });
        record.verified &= stage.verified;
        record.stages.push_back(std::move(stage));
    }

    std::vector<Fp> q_coefficients(checked_product({n, d, d}), Fp::zero()); // [j][w][z]
    std::vector<Fp> z_messages(checked_product({n, n}), Fp::zero());         // [j][v]
    std::atomic<bool> disqualified{false};

    const auto compute_com2_column = [&](std::size_t j, Fp* q_output, Fp* z_output) -> bool {
        std::vector<std::size_t> active;
        active.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            if (!row_valid[i]) continue;
            const std::size_t pair = i * n + j;
            if (digest_slice(
                    sid, i, j, salts[pair],
                    std::span<const Fp>(forwarded_slices.data() + pair * d, d)
                ) == digests[pair]) {
                active.push_back(i);
            }
        }
        if (active.size() < d) return false;

        std::vector<Fp> selected_points;
        selected_points.reserve(d);
        std::vector<Fp> selected(checked_product({d, d}));
        for (std::size_t chosen = 0; chosen < d; ++chosen) {
            selected_points.push_back(points[active[chosen]]);
            const Fp* input = forwarded_slices.data() + (active[chosen] * n + j) * d;
            std::copy_n(input, d, selected.data() + chosen * d);
        }
        const InterpolationPlan local_plan(std::move(selected_points));
        const std::vector<Fp> decoded = local_plan.interpolate_prefix(selected, d, 1);
        std::copy(decoded.begin(), decoded.end(), q_output);

        bool consistent = true;
        for (const std::size_t i : active) {
            const Fp* actual = forwarded_slices.data() + (i * n + j) * d;
            for (std::size_t z = 0; z < d; ++z) {
                Fp expected = Fp::zero();
                for (std::size_t w = d; w-- > 0;) {
                    expected *= points[i];
                    expected += decoded[w * d + z];
                }
                consistent &= expected == actual[z];
            }
        }
        if (!consistent) return false;

        const Fp* q_at_w_zero = decoded.data();
        for (std::size_t v = 0; v < n; ++v) {
            z_output[v] = evaluate_polynomial(
                std::span<const Fp>(q_at_w_zero, d), points[v]
            );
        }
        return true;
    };

    // Representative Com2 party timing. Column zero also
    // exercises the invalid-record path when fault injection is enabled.
    {
        StageSample stage = base_stage(config, record.protocol, record.mode, "com2_party0");
        std::vector<Fp> q_local(d * d, Fp::zero());
        std::vector<Fp> z_local(n, Fp::zero());
        bool valid = false;
        stage.milliseconds = timed([&] {
            valid = compute_com2_column(0, q_local.data(), z_local.data());
        });
        stage.peak_live_bytes = saturated_bytes({n * d, d * d, n});
        stage.communication.p2p_bytes = static_cast<std::uint64_t>(n) * sizeof(Fp);
        const std::uint64_t table_bytes = static_cast<std::uint64_t>(digests.size()) * sizeof(Digest256);
        stage.communication.broadcast_native_bytes = table_bytes + (n + 7) / 8 + sizeof(std::uint32_t);
        stage.communication.broadcast_materialized_bytes = stage.communication.broadcast_native_bytes * n;
        stage.checksum = sampled_checksum(z_local);
        stage.verified = valid;
        record.verified &= stage.verified;
        record.stages.push_back(std::move(stage));
    }

    {
        StageSample stage = base_stage(config, record.protocol, record.mode, "com2_all_parties");
        stage.milliseconds = timed([&] {
            parallel_for(n, threads, [&](std::size_t j) {
                if (!compute_com2_column(
                        j,
                        q_coefficients.data() + j * d * d,
                        z_messages.data() + j * n
                    )) {
                    disqualified.store(true, std::memory_order_relaxed);
                }
            });
        });
        stage.peak_live_bytes = saturated_bytes({forwarded_slices.size(), q_coefficients.size(), z_messages.size()}) +
                                static_cast<std::uint64_t>(digests.size() * sizeof(Digest256));
        stage.communication.p2p_bytes = static_cast<std::uint64_t>(z_messages.size()) * sizeof(Fp);
        const std::uint64_t table_bytes = static_cast<std::uint64_t>(digests.size()) * sizeof(Digest256);
        stage.communication.broadcast_native_bytes = static_cast<std::uint64_t>(n) *
            (table_bytes + (n + 7) / 8 + sizeof(std::uint32_t));
        stage.communication.broadcast_materialized_bytes = stage.communication.broadcast_native_bytes * n;
        stage.checksum = sampled_checksum(z_messages);
        stage.verified = !disqualified.load(std::memory_order_relaxed);
        record.verified &= stage.verified;
        record.stages.push_back(std::move(stage));
    }

    // Representative Com3 party timing.
    {
        StageSample stage = base_stage(config, record.protocol, record.mode, "com3_party0");
        Fp value = Fp::zero();
        stage.milliseconds = timed([&] {
            for (std::size_t j = 0; j < d; ++j) {
                value += plan.zero_weight(j) * z_messages[j * n];
            }
        });
        stage.peak_live_bytes = saturated_bytes({n});
        stage.checksum = value.value();
        stage.verified = !disqualified.load(std::memory_order_relaxed);
        record.verified &= stage.verified;
        record.stages.push_back(std::move(stage));
    }

    std::vector<Fp> final_shares(n, Fp::zero());
    Fp reconstructed = Fp::zero();
    {
        StageSample stage = base_stage(config, record.protocol, record.mode, "com3_share_generation");
        stage.milliseconds = timed([&] {
            parallel_for(n, threads, [&](std::size_t v) {
                Fp value = Fp::zero();
                for (std::size_t j = 0; j < d; ++j) {
                    value += plan.zero_weight(j) * z_messages[j * n + v];
                }
                final_shares[v] = value;
            });
            for (std::size_t v = 0; v < d; ++v) {
                reconstructed += plan.zero_weight(v) * final_shares[v];
            }
        });
        stage.peak_live_bytes = saturated_bytes({z_messages.size(), final_shares.size()});
        stage.checksum = sampled_checksum(final_shares);
        stage.verified = !disqualified.load(std::memory_order_relaxed) && reconstructed == secret;
        record.verified &= stage.verified;
        record.stages.push_back(std::move(stage));
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
        small.include_final_handoff = true;
        if (!run_fdvss1_roles(small).verified) throw std::runtime_error("FDVSS-1 role test failed");
        if (!run_fdvss2(small, false).verified) throw std::runtime_error("FDVSS-2 honest test failed");
        if (!run_fdvss2(small, true).verified) throw std::runtime_error("FDVSS-2 invalid-record test failed");
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
    output << "{\n  \"schema\": \"fdvss-benchmark-v1\",\n  \"system\": \""
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
