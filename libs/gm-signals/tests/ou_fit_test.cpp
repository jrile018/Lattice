#include <gm-signals/ou_fit.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>

using gm::signals::fit_ou;
using gm::signals::ou_zscore;

namespace {

/// Simulates an OU path via its EXACT discretization (not an Euler
/// approximation - the same closed form fit_ou's mapping inverts), so
/// recovery accuracy reflects the fit's own statistical error, not
/// discretization bias from the simulator. A fixed-seed std::mt19937
/// is test-only synthetic data generation, not a violation of ADR Sec 3
/// principle 2 (determinism), which governs production/scored paths -
/// the seed itself makes this test reproducible, which is the point.
Eigen::VectorXd simulate_ou_path(double theta, double mu, double sigma, double dt, int n,
                                  unsigned seed) {
    double phi = std::exp(-theta * dt);
    double noise_std = std::sqrt(sigma * sigma / (2.0 * theta) * (1.0 - phi * phi));

    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0.0, noise_std);

    Eigen::VectorXd path(n);
    path(0) = mu; // start at the long-run mean
    for (int t = 1; t < n; ++t) {
        path(t) = path(t - 1) * phi + mu * (1.0 - phi) + noise(rng);
    }
    return path;
}

} // namespace

TEST_CASE("OU parameters are recovered from a simulated path within statistical tolerance", "[ou]") {
    const double true_theta = 0.1;
    const double true_mu = 2.0;
    const double true_sigma = 0.5;
    const double dt = 1.0;
    const int n = 4000; // long enough for the estimator's sampling error to be small

    Eigen::VectorXd path = simulate_ou_path(true_theta, true_mu, true_sigma, dt, n, /*seed=*/42);

    auto fit = fit_ou(path, dt);
    REQUIRE(fit.has_value());

    // Generous relative/absolute tolerances - this validates the
    // estimator recovers approximately the right regime, not exact
    // equality (which finite-sample noise makes impossible). theta
    // recovery is the least precise of the three (it comes from a log
    // of an estimated correlation), hence the widest band.
    CHECK(fit->theta == Catch::Approx(true_theta).epsilon(0.35));
    CHECK(fit->mu == Catch::Approx(true_mu).margin(0.15));
    CHECK(fit->sigma == Catch::Approx(true_sigma).epsilon(0.15));

    double expected_half_life = std::log(2.0) / true_theta;
    CHECK(fit->half_life == Catch::Approx(expected_half_life).epsilon(0.35));
}

TEST_CASE("a point exactly at the fitted mean has zscore 0", "[ou]") {
    Eigen::VectorXd path = simulate_ou_path(0.1, 2.0, 0.5, 1.0, 3000, /*seed=*/7);
    auto fit = fit_ou(path, 1.0);
    REQUIRE(fit.has_value());

    auto z = ou_zscore(*fit, fit->mu);
    REQUIRE(z.has_value());
    CHECK(*z == Catch::Approx(0.0).margin(1e-9));
}

TEST_CASE("zscore is symmetric and monotonic in the deviation from the mean", "[ou]") {
    Eigen::VectorXd path = simulate_ou_path(0.1, 2.0, 0.5, 1.0, 3000, /*seed=*/7);
    auto fit = fit_ou(path, 1.0);
    REQUIRE(fit.has_value());

    auto z_above = ou_zscore(*fit, fit->mu + 1.0);
    auto z_below = ou_zscore(*fit, fit->mu - 1.0);
    REQUIRE(z_above.has_value());
    REQUIRE(z_below.has_value());
    CHECK(*z_above == Catch::Approx(-*z_below).margin(1e-9));
    CHECK(*z_above > 0.0);
    CHECK(*z_below < 0.0);
}

TEST_CASE("a constant series is rejected (undefined AR(1) slope)", "[ou]") {
    Eigen::VectorXd path = Eigen::VectorXd::Constant(10, 5.0);
    auto fit = fit_ou(path, 1.0);
    REQUIRE_FALSE(fit.has_value());
}

TEST_CASE("a perfectly alternating series is rejected (phi <= 0, not mean-reverting)", "[ou]") {
    // s = [0, 1, 0, 1, 0, 1, ...] has lag-1 autocorrelation of -1 -
    // phi_hat = -1, outside the required (0,1) range.
    Eigen::VectorXd path(10);
    for (int i = 0; i < 10; ++i) path(i) = (i % 2 == 0) ? 0.0 : 1.0;
    auto fit = fit_ou(path, 1.0);
    REQUIRE_FALSE(fit.has_value());
}

TEST_CASE("a monotonically trending series is rejected (phi >= 1, non-stationary)", "[ou]") {
    Eigen::VectorXd path(10);
    for (int i = 0; i < 10; ++i) path(i) = static_cast<double>(i); // pure linear trend, phi_hat ~ 1
    auto fit = fit_ou(path, 1.0);
    REQUIRE_FALSE(fit.has_value());
}

TEST_CASE("fewer than 3 points is rejected", "[ou]") {
    Eigen::VectorXd path(2);
    path << 1.0, 2.0;
    auto fit = fit_ou(path, 1.0);
    REQUIRE_FALSE(fit.has_value());
}

TEST_CASE("nonpositive dt is rejected", "[ou]") {
    Eigen::VectorXd path = simulate_ou_path(0.1, 2.0, 0.5, 1.0, 10, /*seed=*/1);
    auto fit = fit_ou(path, 0.0);
    REQUIRE_FALSE(fit.has_value());
}
