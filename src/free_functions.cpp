#include <algorithm>
#include <cmath>

bool nearly_equal(double a, double b,
                  double abs_tol = 1e-9,
                  double rel_tol = 1e-12) {
    return std::abs(a - b) <=
        std::max(abs_tol,
                 rel_tol * std::max(std::abs(a), std::abs(b)));
}