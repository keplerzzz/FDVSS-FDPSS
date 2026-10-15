#include "fdvss/reference.hpp"

#include <stdexcept>

namespace {

using fdvss::Fp;
using namespace fdvss::reference;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Decoded known_polynomial(std::size_t t, std::size_t width) {
    Decoded polynomial;
    polynomial.width = width;
    for (std::size_t degree = 0; degree <= t; ++degree) {
        for (std::size_t coordinate = 0; coordinate < width; ++coordinate) {
            polynomial.coefficients.push_back(Fp::from_u64(11 + degree * 19 + coordinate * 7));
        }
    }
    return polynomial;
}

std::vector<Record> encoded(const Decoded& polynomial, std::size_t n) {
    std::vector<Record> values(n);
    for (std::size_t i = 0; i < n; ++i) values[i] = evaluate(polynomial, Fp::from_u64(i + 1));
    return values;
}

void test_errors_and_erasures() {
    const auto expected = known_polynomial(3, 3);
    auto values = encoded(expected, 10);
    for (std::size_t i = 0; i < 3; ++i) {
        for (auto& value : *values[i]) value += Fp::from_u64(123 + i);
    }
    auto recovered = decode(values, 3, 7);
    require(recovered && recovered->coefficients == expected.coefficients, "threshold errors failed");
    require(recovered->supporters.size() == 7 && recovered->supporters.front() == 3, "wrong complete supporters");
    require(reconstruct(values, 3, 7) == Record(evaluate(expected, Fp::zero())), "reconstruction failed");

    values[0].reset();
    recovered = decode(values, 3, 7);
    require(recovered && recovered->coefficients == expected.coefficients, "mixed errors and erasures failed");
    values[1].reset();
    values[2].reset();
    recovered = decode(values, 3, 7);
    require(recovered && recovered->supporters.size() == 7, "threshold erasures failed");
    values[3].reset();
    require(!decode(values, 3, 7), "too few supporters accepted");
}

void test_complete_record_support() {
    auto values = encoded(known_polynomial(3, 2), 10);
    for (std::size_t i = 0; i < 3; ++i) (*values[i])[0] += Fp::one();
    for (std::size_t i = 3; i < 6; ++i) (*values[i])[1] += Fp::one();
    require(!decode(values, 3, 7), "different coordinate supporter sets accepted");

    values = encoded(known_polynomial(1, 2), 4);
    values[1] = Polynomial{Fp::one()};
    require(decode(values, 1, 3).has_value(), "wrong-width record was not erased");
    values[2] = Polynomial{Fp::one()};
    require(!decode(values, 1, 3), "too many malformed records accepted");
}

void test_exact_interpolation() {
    const auto expected = known_polynomial(2, 2);
    auto values = encoded(expected, 7);
    values[1].reset();
    auto recovered = interpolate(values, 2);
    require(recovered && recovered->coefficients == expected.coefficients, "exact interpolation failed");
    (*values[6])[1] += Fp::one();
    require(!interpolate(values, 2), "inconsistent exact interpolation accepted");
    values = encoded(expected, 2);
    require(!interpolate(values, 2), "underdetermined interpolation accepted");
    require(!decode(encoded(known_polynomial(1, 1), 4), 1, 2), "ambiguous support threshold accepted");
}

void test_zero_and_empty_records() {
    Decoded zero{Polynomial(6, Fp::zero()), {}, 2};
    auto recovered = decode(encoded(zero, 4), 2, 4);
    require(recovered && recovered->coefficients == zero.coefficients, "zero polynomial failed");
    std::vector<Record> values(4, Polynomial{});
    values[2].reset();
    recovered = decode(values, 1, 3);
    require(recovered && recovered->width == 0 && recovered->coefficients.empty(), "empty tuple failed");
    require(recovered->supporters == std::vector<std::size_t>({0, 1, 3}), "empty tuple supporters failed");
    require(reconstruct(values, 1, 3) == Record(Polynomial{}), "empty tuple reconstruction failed");
    require(decode(values, 1, 1).has_value(), "empty tuple incorrectly required scalar uniqueness");
    require(!decode(std::vector<Record>(4), 1, 3), "all erased records accepted");
    require(!decode({}, 0, 1), "empty input accepted");
    require(!decode(std::vector<Record>(2, Polynomial{}), 2, 2), "empty tuple bypassed threshold");

    bool threw = false;
    try { (void)evaluate(Decoded{{Fp::one()}, {}, 0}, Fp::zero()); }
    catch (const std::invalid_argument&) { threw = true; }
    require(threw, "invalid polynomial layout accepted");
}

}  // namespace

void reference_algebra_tests() {
    test_errors_and_erasures();
    test_complete_record_support();
    test_exact_interpolation();
    test_zero_and_empty_records();
}
