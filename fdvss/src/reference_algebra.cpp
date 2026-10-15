#include "fdvss/reference.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace fdvss::reference {
namespace {

struct Available {
    std::size_t width = 0;
    std::vector<std::size_t> indices;
};

std::optional<Available> available_records(const std::vector<Record>& values) {
    Available result;
    bool found = false;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!values[i]) continue;
        if (!found) {
            result.width = values[i]->size();
            found = true;
        }
        if (values[i]->size() == result.width) result.indices.push_back(i);
    }
    if (!found) return std::nullopt;
    return result;
}

// Solve an overdetermined system exactly. Free variables are set to zero.
std::optional<Polynomial> solve(std::vector<Polynomial> matrix, std::size_t variables) {
    std::vector<std::size_t> pivot_columns;
    std::size_t pivot_row = 0;
    for (std::size_t column = 0; column < variables && pivot_row < matrix.size(); ++column) {
        std::size_t selected = pivot_row;
        while (selected < matrix.size() && matrix[selected][column].is_zero()) ++selected;
        if (selected == matrix.size()) continue;
        std::swap(matrix[selected], matrix[pivot_row]);
        const Fp inverse = matrix[pivot_row][column].inverse();
        for (std::size_t k = column; k <= variables; ++k) matrix[pivot_row][k] *= inverse;
        for (std::size_t row = 0; row < matrix.size(); ++row) {
            if (row == pivot_row || matrix[row][column].is_zero()) continue;
            const Fp multiple = matrix[row][column];
            for (std::size_t k = column; k <= variables; ++k) {
                matrix[row][k] -= multiple * matrix[pivot_row][k];
            }
        }
        pivot_columns.push_back(column);
        ++pivot_row;
    }
    for (const auto& row : matrix) {
        const bool zero = std::all_of(row.begin(), row.begin() + variables,
                                      [](Fp value) { return value.is_zero(); });
        if (zero && !row[variables].is_zero()) return std::nullopt;
    }
    Polynomial solution(variables, Fp::zero());
    for (std::size_t row = 0; row < pivot_columns.size(); ++row) {
        solution[pivot_columns[row]] = matrix[row][variables];
    }
    return solution;
}

// Monic Berlekamp-Welch locator of degree errors. A rank-deficient locator
// still gives the same quotient within the unique-decoding radius.
std::optional<Polynomial> decode_coordinate(
    const std::vector<Record>& values, const Available& available,
    std::size_t coordinate, std::size_t t, std::size_t errors
) {
    const std::size_t q_width = t + errors + 1;
    const std::size_t variables = q_width + errors;
    std::vector<Polynomial> matrix;
    matrix.reserve(available.indices.size());
    for (const std::size_t i : available.indices) {
        const Fp point = Fp::from_u64(i + 1);
        const Fp value = (*values[i])[coordinate];
        Polynomial powers(q_width, Fp::one());
        for (std::size_t k = 1; k < q_width; ++k) powers[k] = powers[k - 1] * point;
        Polynomial row(variables + 1, Fp::zero());
        std::copy(powers.begin(), powers.end(), row.begin());
        for (std::size_t k = 0; k < errors; ++k) row[q_width + k] = Fp::zero() - value * powers[k];
        row[variables] = value * powers[errors];
        matrix.push_back(std::move(row));
    }
    auto solution = solve(std::move(matrix), variables);
    if (!solution) return std::nullopt;
    Polynomial locator(errors + 1, Fp::one());
    for (std::size_t k = 0; k < errors; ++k) locator[k] = (*solution)[q_width + k];
    Polynomial remainder(solution->begin(), solution->begin() + q_width);
    Polynomial quotient(t + 1, Fp::zero());
    for (std::size_t k = q_width; k-- > errors;) {
        const Fp leading = remainder[k];
        quotient[k - errors] = leading;
        for (std::size_t j = 0; j <= errors; ++j) remainder[k - errors + j] -= leading * locator[j];
    }
    if (!std::all_of(remainder.begin(), remainder.end(), [](Fp value) { return value.is_zero(); })) {
        return std::nullopt;
    }
    return quotient;
}

}  // namespace

Polynomial evaluate(const Decoded& polynomial, Fp point) {
    if (polynomial.width == 0) {
        if (!polynomial.coefficients.empty()) throw std::invalid_argument("nonempty zero-width polynomial");
        return {};
    }
    if (polynomial.coefficients.empty() || polynomial.coefficients.size() % polynomial.width != 0) {
        throw std::invalid_argument("invalid polynomial coefficient layout");
    }
    Polynomial result(polynomial.width, Fp::zero());
    const std::size_t degrees = polynomial.coefficients.size() / polynomial.width;
    for (std::size_t degree = degrees; degree-- > 0;) {
        for (std::size_t coordinate = 0; coordinate < polynomial.width; ++coordinate) {
            result[coordinate] *= point;
            result[coordinate] += polynomial.coefficients[degree * polynomial.width + coordinate];
        }
    }
    return result;
}

std::optional<Decoded> decode(
    const std::vector<Record>& values, std::size_t t, std::size_t required_support
) {
    const auto available = available_records(values);
    if (!available) return std::nullopt;
    const std::size_t count = available->indices.size();
    if (t >= count || required_support > count) return std::nullopt;
    if (available->width == 0) return Decoded{{}, available->indices, 0};
    if (required_support <= t) return std::nullopt;
    const std::size_t errors = count - required_support;
    // Equivalent to count >= t + 2*errors + 1, without overflowing.
    if (errors > (count - t - 1) / 2) return std::nullopt;
    if (available->width != 0 && t + 1 > std::numeric_limits<std::size_t>::max() / available->width) {
        return std::nullopt;
    }
    Decoded result;
    result.width = available->width;
    result.coefficients.resize((t + 1) * result.width, Fp::zero());
    for (std::size_t coordinate = 0; coordinate < result.width; ++coordinate) {
        const auto scalar = decode_coordinate(values, *available, coordinate, t, errors);
        if (!scalar) return std::nullopt;
        for (std::size_t degree = 0; degree <= t; ++degree) {
            result.coefficients[degree * result.width + coordinate] = (*scalar)[degree];
        }
    }
    // Coordinates cannot choose different supporter sets: authenticate the
    // entire tuple against one vector polynomial after scalar decoding.
    for (const std::size_t i : available->indices) {
        if (evaluate(result, Fp::from_u64(i + 1)) == *values[i]) result.supporters.push_back(i);
    }
    if (result.supporters.size() < required_support) return std::nullopt;
    return result;
}

std::optional<Decoded> interpolate(const std::vector<Record>& values, std::size_t t) {
    const auto available = available_records(values);
    if (!available) return std::nullopt;
    return decode(values, t, available->indices.size());
}

Record reconstruct(const std::vector<Record>& values, std::size_t t, std::size_t required_support) {
    const auto polynomial = decode(values, t, required_support);
    if (!polynomial) return std::nullopt;
    return evaluate(*polynomial, Fp::zero());
}

}  // namespace fdvss::reference
