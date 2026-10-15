#include "fdvss/field.hpp"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

using u64 = std::uint64_t;
using u128 = unsigned __int128;
constexpr u64 p = 0xffffffff00000001ULL;
static u64 pairs = 0;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void check_pair(u64 raw_a, u64 raw_b) {
    const auto a = fdvss::Fp::from_u64(raw_a);
    const auto b = fdvss::Fp::from_u64(raw_b);
    const u64 ar = raw_a % p;
    const u64 br = raw_b % p;
    require(a.value() == ar && b.value() == br, "input reduction mismatch");
    const auto sum = a + b;
    const auto difference = a - b;
    const auto product = a * b;
    require(sum.value() < p && difference.value() < p && product.value() < p,
            "noncanonical output");
    require(sum.value() == (u128(ar) + br) % p, "addition mismatch");
    require(difference.value() == (u128(ar) + p - br) % p, "subtraction mismatch");
    require(product.value() == u128(ar) * br % p, "multiplication mismatch");
    ++pairs;
}

int main() {
    std::vector<u64> edges = {0, 1, 2, 3, p, p - 1, p - 2, p - 3,
        p + 1, p + 2, p + 3, std::numeric_limits<u64>::max(),
        std::numeric_limits<u64>::max() - 1};
    for (unsigned bit = 1; bit < 64; ++bit) {
        const u64 value = u64{1} << bit;
        for (const u64 x : {value - 1, value, value + 1}) {
            edges.push_back(x);
            edges.push_back(p - x);
        }
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    for (u64 a : edges) for (u64 b : edges) check_pair(a, b);
    const auto edge_pairs = pairs;
    std::mt19937_64 rng(0x20261003);
    for (u64 trial = 0; trial < 1000000; ++trial) check_pair(rng(), rng());
    std::size_t inversion_tests = 0;
    for (std::size_t trial = 0; trial < 10000; ++trial) {
        const auto a = fdvss::Fp::from_u64(rng());
        if (a.is_zero()) continue;
        const auto inverse = a.inverse();
        require(inverse.value() < p, "noncanonical inverse");
        require(u128(a.value()) * inverse.value() % p == 1, "inverse mismatch");
        ++inversion_tests;
    }
    bool zero_inverse_rejected = false;
    try { (void)fdvss::Fp::zero().inverse(); }
    catch (const std::domain_error &) { zero_inverse_rejected = true; }
    require(zero_inverse_rejected, "zero inverse accepted");
    std::cout << "PASS: " << edges.size() << " unique edge values; "
              << edge_pairs << " edge pairs; 1000000 random pairs; "
              << pairs * 3 << " arithmetic comparisons; " << inversion_tests
              << " inversions; zero inverse rejected.\n";
}
