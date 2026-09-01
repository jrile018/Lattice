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

TEST_CASE("FastMCD is deterministic on data in different order", "[fastmcd]") {
    Eigen::MatrixXd points1(10, 2);
    points1.col(0) << -2, -1.5, -1, -0.5, 0, 0.5, 1, 1.5, 2, 0;
    points1.col(1) << -1, -0.8, -0.5, -0.2, 0, 0.2, 0.5, 0.8, 1, 0.1;

    Eigen::MatrixXd points2(10, 2);
    points2 << points1.row(5),   points1.row(2),   points1.row(9),   points1.row(0),   points1.row(7),
              points1.row(1),   points1.row(4),   points1.row(8),   points1.row(3),   points1.row(6);

    auto fit1 = fit_fastmcd(points1);
    REQUIRE(fit1.has_value());

    auto fit2 = fit_fastmcd(points2);
    REQUIRE(fit2.has_value());

    CHECK((fit1->location - fit2->location).norm() < 0.2);
    CHECK((fit1->covariance - fit2->covariance).norm() < 0.1);
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
