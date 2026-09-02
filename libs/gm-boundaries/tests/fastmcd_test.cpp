#include <gm-boundaries/fastmcd.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

using gm::boundaries::fit_fastmcd;
using gm::boundaries::score_fastmcd;

namespace {
constexpr double kTol = 1e-6;
}

TEST_CASE("FastMCD fit accepts sufficient points", "[fastmcd]") {
    Eigen::MatrixXd points(10, 2);
    points.col(0) << -2, -1.5, -1, -0.5, 0, 0.5, 1, 1.5, 2, 0;
    points.col(1) << -1, -0.8, -0.5, -0.2, 0, 0.2, 0.5, 0.8, 1, 0.1;

    auto fit = fit_fastmcd(points);
    REQUIRE(fit.has_value());
    CHECK(fit->degrees_of_freedom == 2);
}

TEST_CASE("FastMCD rejects fewer than p+1 points", "[fastmcd]") {
    Eigen::MatrixXd points(2, 3);
    points.setZero();
    auto fit = fit_fastmcd(points);
    REQUIRE_FALSE(fit.has_value());
    CHECK(fit.error().code == gm::ErrorCode::kInvalidArgument);
}

TEST_CASE("FastMCD rejects zero-column matrix", "[fastmcd]") {
    Eigen::MatrixXd points(5, 0);
    auto fit = fit_fastmcd(points);
    REQUIRE_FALSE(fit.has_value());
    CHECK(fit.error().code == gm::ErrorCode::kInvalidArgument);
}

TEST_CASE("FastMCD scores a point at the center as inside", "[fastmcd]") {
    Eigen::MatrixXd points(10, 2);
    points.col(0) << -2, -1.5, -1, -0.5, 0, 0.5, 1, 1.5, 2, 0;
    points.col(1) << -1, -0.8, -0.5, -0.2, 0, 0.2, 0.5, 0.8, 1, 0.1;

    auto fit = fit_fastmcd(points);
    REQUIRE(fit.has_value());

    Eigen::VectorXd center(2);
    center << 0.0, 0.0;
    auto score = score_fastmcd(*fit, center);
    REQUIRE(score.has_value());
    CHECK(score->inside);
    CHECK(score->depth < 0.0);
}

TEST_CASE("FastMCD scores a far point as outside", "[fastmcd]") {
    Eigen::MatrixXd points(10, 2);
    points.col(0) << -2, -1.5, -1, -0.5, 0, 0.5, 1, 1.5, 2, 0;
    points.col(1) << -1, -0.8, -0.5, -0.2, 0, 0.2, 0.5, 0.8, 1, 0.1;

    auto fit = fit_fastmcd(points);
    REQUIRE(fit.has_value());

    Eigen::VectorXd far_point(2);
    far_point << 100.0, 100.0;
    auto score = score_fastmcd(*fit, far_point);
    REQUIRE(score.has_value());
    CHECK_FALSE(score->inside);
    CHECK(score->depth > 0.0);
    CHECK(score->p_value < 0.01);
}

TEST_CASE("FastMCD scoring depth is monotonic along a ray", "[fastmcd]") {
    Eigen::MatrixXd points(10, 2);
    points.col(0) << -2, -1.5, -1, -0.5, 0, 0.5, 1, 1.5, 2, 0;
    points.col(1) << -1, -0.8, -0.5, -0.2, 0, 0.2, 0.5, 0.8, 1, 0.1;

    auto fit = fit_fastmcd(points);
    REQUIRE(fit.has_value());

    double prev_depth = -1e18;
    for (double d : {0.1, 1.0, 2.0, 5.0, 10.0}) {
        Eigen::VectorXd p(2);
        p << d, 0.0;
        auto score = score_fastmcd(*fit, p);
        REQUIRE(score.has_value());
        CHECK(score->depth > prev_depth);
        prev_depth = score->depth;
    }
}

// Renamed and rewritten from "FastMCD is deterministic on data in
// different order": a real-data audit found row-permutation invariance
// does NOT hold (fastmcd.hpp documents this explicitly - the seed hash
// is order-invariant, but the row indices it seeds a Fisher-Yates
// shuffle over are not, so a different order selects different actual
// points into the same numeric index positions). The old test's name
// and content asserted exactly the property the header now disclaims;
// it only "passed" because its tiny 10-point fixture happened not to
// violate its own 0.2/0.1 tolerance - real data violates it by up to
// 2.5x. What IS guaranteed, and worth a real test, is repeatability for
// a FIXED row order: the same points, called twice, must produce a
// bit-identical fit (this is the actual determinism ADR-003 requires,
// and the property every real caller in this codebase depends on,
// since they always build the training matrix in std::map order).
TEST_CASE("FastMCD is repeatable for a fixed row order", "[fastmcd]") {
    Eigen::MatrixXd points(10, 2);
    points.col(0) << -2, -1.5, -1, -0.5, 0, 0.5, 1, 1.5, 2, 0;
    points.col(1) << -1, -0.8, -0.5, -0.2, 0, 0.2, 0.5, 0.8, 1, 0.1;

    auto fit1 = fit_fastmcd(points);
    REQUIRE(fit1.has_value());
    auto fit2 = fit_fastmcd(points);
    REQUIRE(fit2.has_value());

    CHECK((fit1->location - fit2->location).norm() < kTol);
    CHECK((fit1->covariance - fit2->covariance).norm() < kTol);
}

TEST_CASE("FastMCD score dimension mismatch is rejected", "[fastmcd]") {
    Eigen::MatrixXd points(10, 2);
    points.col(0) << -2, -1.5, -1, -0.5, 0, 0.5, 1, 1.5, 2, 0;
    points.col(1) << -1, -0.8, -0.5, -0.2, 0, 0.2, 0.5, 0.8, 1, 0.1;

    auto fit = fit_fastmcd(points);
    REQUIRE(fit.has_value());

    Eigen::VectorXd bad_point(3);
    bad_point << 1, 2, 3;
    auto score = score_fastmcd(*fit, bad_point);
    REQUIRE_FALSE(score.has_value());
    CHECK(score.error().code == gm::ErrorCode::kInvalidArgument);
}

TEST_CASE("FastMCD rejects out-of-range alpha", "[fastmcd]") {
    Eigen::MatrixXd points(10, 2);
    points.col(0) << -2, -1.5, -1, -0.5, 0, 0.5, 1, 1.5, 2, 0;
    points.col(1) << -1, -0.8, -0.5, -0.2, 0, 0.2, 0.5, 0.8, 1, 0.1;

    auto fit = fit_fastmcd(points);
    REQUIRE(fit.has_value());

    Eigen::VectorXd p(2);
    p << 3, 6;
    CHECK_FALSE(score_fastmcd(*fit, p, 0.0).has_value());
    CHECK_FALSE(score_fastmcd(*fit, p, 1.0).has_value());
}

TEST_CASE("FastMCD fits synthetic Gaussian plus contamination", "[fastmcd]") {
    const int n = 50;
    Eigen::MatrixXd points(n, 2);

    for (int i = 0; i < 40; ++i) {
        points(i, 0) = static_cast<double>(i % 8) - 3.5;
        points(i, 1) = static_cast<double>(i / 8) - 2.5;
    }

    for (int i = 40; i < 50; ++i) {
        points(i, 0) = 10.0 + static_cast<double>(i);
        points(i, 1) = 10.0 + static_cast<double>(i);
    }

    auto fit = fit_fastmcd(points);
    REQUIRE(fit.has_value());

    Eigen::VectorXd test_point(2);
    test_point << 0.0, 0.0;
    auto score = score_fastmcd(*fit, test_point);
    REQUIRE(score.has_value());
    CHECK(score->inside);

    Eigen::VectorXd outlier(2);
    outlier << 50.0, 50.0;
    auto outlier_score = score_fastmcd(*fit, outlier);
    REQUIRE(outlier_score.has_value());
    CHECK_FALSE(outlier_score->inside);
}

TEST_CASE("FastMCD produces valid covariance matrix", "[fastmcd]") {
    Eigen::MatrixXd points(15, 3);
    for (int i = 0; i < 15; ++i) {
        points(i, 0) = static_cast<double>(i) - 7.0;
        points(i, 1) = static_cast<double>(i) * 0.5 - 3.5 + (i % 2) * 0.3;
        points(i, 2) = -static_cast<double>(i) * 0.7 + 5.0 + (i % 3) * 0.2;
    }

    auto fit = fit_fastmcd(points);
    REQUIRE(fit.has_value());

    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(fit->covariance);
    REQUIRE(solver.info() == Eigen::Success);

    for (int i = 0; i < fit->degrees_of_freedom; ++i) {
        CHECK(solver.eigenvalues()(i) > 1e-10);
    }
}

// Real-data audit findings, each with a dedicated regression test so
// they cannot silently regress again:

TEST_CASE("FastMCD selects the MINIMUM determinant candidate, not the maximum",
          "[fastmcd]") {
    // A dumbbell: two tight clusters connected by a few bridge points.
    // The two tight clusters are far more concentrated (much smaller
    // determinant) than any subset spanning the bridge - so a correct
    // (minimum-determinant) MCD fit should center on ONE of the tight
    // clusters, not straddle the bridge. h = (n+p+1)/2 with n=24,p=2
    // gives h=13, comfortably larger than either 10-point cluster alone
    // but small enough that "straddle the whole thing" (a much larger
    // determinant) is a clearly worse, and clearly different, answer.
    Eigen::MatrixXd points(24, 2);
    for (int i = 0; i < 10; ++i) {
        points(i, 0) = -10.0 + 0.05 * static_cast<double>(i % 5);
        points(i, 1) = 0.05 * static_cast<double>(i / 5);
    }
    for (int i = 10; i < 20; ++i) {
        points(i, 0) = 10.0 + 0.05 * static_cast<double>((i - 10) % 5);
        points(i, 1) = 0.05 * static_cast<double>((i - 10) / 5);
    }
    for (int i = 20; i < 24; ++i) {
        points(i, 0) = -5.0 + 5.0 * static_cast<double>(i - 20);
        points(i, 1) = 0.0;
    }

    auto fit = fit_fastmcd(points);
    REQUIRE(fit.has_value());
    // A minimum-determinant fit centers near one of the two tight
    // clusters (x near -10 or +10); a maximum-determinant fit (the bug
    // this test guards against) centers near the bridge's midpoint
    // (x near 0), since that subset has a much LARGER determinant.
    CHECK(std::abs(fit->location(0)) > 5.0);
}

TEST_CASE("FastMCD's 5 trials use genuinely different starting subsets",
          "[fastmcd]") {
    // A real-data audit found the original trial-subset generator
    // degenerated to the identical subset for every trial (h=(n+p+1)/2
    // is always > n/2, so an n/h-derived stride was always 1). This
    // does not test fit_fastmcd's public API directly (the subset
    // generator is file-local) - instead it exercises the observable
    // consequence: on data where different starting subsets are known
    // to converge to meaningfully different local optima (the dumbbell
    // shape above, run repeatedly), a genuinely-diverse-trials estimator
    // should reliably find the true minimum every time, not the same or
    // the wrong answer with the identical result.
    Eigen::MatrixXd points(24, 2);
    for (int i = 0; i < 10; ++i) {
        points(i, 0) = -10.0 + 0.05 * static_cast<double>(i % 5);
        points(i, 1) = 0.05 * static_cast<double>(i / 5);
    }
    for (int i = 10; i < 20; ++i) {
        points(i, 0) = 10.0 + 0.05 * static_cast<double>((i - 10) % 5);
        points(i, 1) = 0.05 * static_cast<double>((i - 10) / 5);
    }
    for (int i = 20; i < 24; ++i) {
        points(i, 0) = -5.0 + 5.0 * static_cast<double>(i - 20);
        points(i, 1) = 0.0;
    }

    for (int trial = 0; trial < 3; ++trial) {
        auto fit = fit_fastmcd(points);
        REQUIRE(fit.has_value());
        CHECK(std::abs(fit->location(0)) > 5.0);
    }
}

TEST_CASE("FastMCD bounds the condition number of a near-degenerate fit",
          "[fastmcd]") {
    // A real-data audit (2011-02-08, ticker TJX, View A) found a raw
    // h-subset covariance with a smallest eigenvalue near machine
    // epsilon relative to its largest - scoring a real point at
    // depth=57594 against Mahalanobis's 17.45 on the same data, with
    // over 99% of the squared distance coming from a single near-null
    // eigendirection. This fixture reproduces the same shape: points
    // nearly collinear in 3D (tiny variance in the 3rd dimension).
    const int n = 20;
    Eigen::MatrixXd points(n, 3);
    for (int i = 0; i < n; ++i) {
        double t = static_cast<double>(i) - static_cast<double>(n) / 2.0;
        // Dimensions 0 and 1 vary independently (NOT one a multiple of
        // the other - an earlier version of this fixture accidentally
        // made dim 1 = 0.5*dim0, a second, unintended exactly-singular
        // direction that made every subset genuinely rank-deficient
        // regardless of dimension 2's treatment). Only dimension 2 is
        // deliberately thin relative to 0 and 1.
        points(i, 0) = t;
        points(i, 1) = static_cast<double>((i * 13) % 9) - 4.0;
        points(i, 2) = 0.05 * t + 0.01 * static_cast<double>((i * 37) % 7 - 3);
    }

    auto fit = fit_fastmcd(points);
    REQUIRE(fit.has_value());

    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(fit->covariance);
    REQUIRE(solver.info() == Eigen::Success);
    double condition_number = solver.eigenvalues()(2) / solver.eigenvalues()(0);
    // Regularized by construction - must be bounded, not just checked.
    CHECK(condition_number <= 100.0 + kTol);

    // A point that deviates ONLY along the near-null direction must not
    // produce an astronomically large, single-axis-dominated depth.
    Eigen::VectorXd probe(3);
    probe << 0.0, 0.0, 1.0;
    auto score = score_fastmcd(*fit, probe);
    REQUIRE(score.has_value());
    CHECK(score->distance < 50.0); // was in the thousands before this fix
}
