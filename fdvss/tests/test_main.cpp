#include "fdvss/core.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool fdvss1_happy_path_end_to_end() {
    constexpr std::size_t n = 4;
    constexpr std::size_t t = 1;
    constexpr std::size_t d = t + 1;
    const auto points = fdvss::evaluation_points(n);
    const fdvss::InterpolationPlan plan(d);
    const fdvss::Fp secret = fdvss::Fp::from_u64(0x5a17);

    // Dense F(W,X,Y,Z), indexed [w][x][y][z].
    std::vector<fdvss::Fp> polynomial(d * d * d * d);
    fdvss::SplitMix64 random(20260923);
    for (auto& value : polynomial) value = random.next_field();
    polynomial[0] = secret;

    // U_{i,k}(0,Z) = F(i,0,k,Z), the happy-row value carried by Com3.
    const std::size_t secret_columns = n * n * d;
    std::vector<fdvss::Fp> u_coefficients(secret_columns, fdvss::Fp::zero());
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t k = 0; k < n; ++k) {
            for (std::size_t z = 0; z < d; ++z) {
                fdvss::Fp over_w = fdvss::Fp::zero();
                for (std::size_t w = d; w-- > 0;) {
                    fdvss::Fp over_y = fdvss::Fp::zero();
                    for (std::size_t y = d; y-- > 0;) {
                        const std::size_t index = ((w * d) * d + y) * d + z; // x=0
                        over_y *= points[k];
                        over_y += polynomial[index];
                    }
                    over_w *= points[i];
                    over_w += over_y;
                }
                u_coefficients[(i * n + k) * d + z] = over_w;
            }
        }
    }

    // Coefficientwise Shamir sharing from each Com3 k to every Com4 l.
    std::vector<fdvss::Fp> sharing_coefficients(d * secret_columns);
    for (auto& value : sharing_coefficients) value = random.next_field();
    std::copy(u_coefficients.begin(), u_coefficients.end(), sharing_coefficients.begin());
    const auto h1 = fdvss::evaluate_matrix(
        sharing_coefficients, d, secret_columns, points, 2
    ); // [l][i][k][z]

    std::vector<fdvss::Fp> final_shares(n, fdvss::Fp::zero());
    for (std::size_t v = 0; v < n; ++v) {
        std::vector<fdvss::Fp> r_i(n, fdvss::Fp::zero());
        for (std::size_t i = 0; i < n; ++i) {
            std::vector<fdvss::Fp> over_k(n, fdvss::Fp::zero());
            for (std::size_t k = 0; k < n; ++k) {
                for (std::size_t l = 0; l < d; ++l) {
                    fdvss::Fp evaluated_z = fdvss::Fp::zero();
                    for (std::size_t z = d; z-- > 0;) {
                        evaluated_z *= points[v];
                        evaluated_z += h1[l * secret_columns + (i * n + k) * d + z];
                    }
                    over_k[k] += plan.zero_weight(l) * evaluated_z;
                }
            }
            for (std::size_t k = 0; k < d; ++k) {
                r_i[i] += plan.zero_weight(k) * over_k[k];
            }
        }
        for (std::size_t i = 0; i < d; ++i) {
            final_shares[v] += plan.zero_weight(i) * r_i[i];
        }
    }

    fdvss::Fp reconstructed = fdvss::Fp::zero();
    for (std::size_t v = 0; v < d; ++v) {
        reconstructed += plan.zero_weight(v) * final_shares[v];
    }
    return reconstructed == secret;
}

}  // namespace

int main() {
    std::string failure;
    if (!fdvss::self_test(&failure)) {
        std::cerr << failure << '\n';
        return 1;
    }
    if (!fdvss1_happy_path_end_to_end()) {
        std::cerr << "FDVSS-1 happy-path end-to-end reconstruction failed\n";
        return 1;
    }
    std::cout << "all tests passed\n";
    return 0;
}
