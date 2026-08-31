#include <gm-boundaries/fastmcd.hpp>
#include <boost/math/distributions/chi_squared.hpp>
#include <algorithm>
#include <cmath>
#include <exception>
#include <numeric>
#include <vector>

namespace gm::boundaries {
namespace {
constexpr double kMinEigenvalue = 1e-10;
constexpr double kCstepTolerance = 1e-9;
constexpr int kMaxCstepIterations = 100;
constexpr int kNumInitialTrials = 5;

std::uint32_t compute_data_seed(const Eigen::MatrixXd& points) {
    std::vector<double> sorted_first_col(static_cast<std::size_t>(points.rows()));
    for (int i = 0; i < points.rows(); ++i) {
        sorted_first_col[static_cast<std::size_t>(i)] = points(i, 0);
    }
    std::sort(sorted_first_col.begin(), sorted_first_col.end());
    std::uint32_t hash = 0;
    for (double val : sorted_first_col) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &val, std::min(sizeof(bits), sizeof(val)));
        hash ^= bits;
        hash = (hash << 5) | (hash >> 27);
    }
    return hash;
}

std::vector<int> deterministic_h_subset(int n, int h, int trial_num, std::uint32_t seed) {
    std::uint32_t trial_hash = seed ^ static_cast<std::uint32_t>(trial_num);
    int stride = 1 + static_cast<int>(trial_hash % std::max(1u, static_cast<std::uint32_t>(n / h)));
    int offset = static_cast<int>((trial_hash / std::max(1u, static_cast<std::uint32_t>(n / h))) % std::max(1u, static_cast<std::uint32_t>(stride)));
    std::vector<int> subset;
    subset.reserve(static_cast<std::size_t>(h));
    for (int i = offset; i < n && static_cast<int>(subset.size()) < h; i += stride) {
        subset.push_back(i);
    }
    if (static_cast<int>(subset.size()) < h) {
        for (int i = 0; i < n && static_cast<int>(subset.size()) < h; ++i) {
            if (std::find(subset.begin(), subset.end(), i) == subset.end()) {
                subset.push_back(i);
            }
        }
    }
    return subset;
}

struct McdCandidate {
    Eigen::VectorXd location;
    Eigen::MatrixXd covariance;
    double log_determinant;
    int converged_iterations;
};

bool compute_mcd_statistics(const Eigen::MatrixXd& points, const std::vector<int>& subset,
                            Eigen::VectorXd& location, Eigen::MatrixXd& covariance) {
    int h = static_cast<int>(subset.size());
    int p = static_cast<int>(points.cols());
    Eigen::MatrixXd subset_matrix(h, p);
    for (int i = 0; i < h; ++i) {
        subset_matrix.row(i) = points.row(static_cast<Eigen::Index>(subset[static_cast<std::size_t>(i)]));
    }
    location = subset_matrix.colwise().mean();
    Eigen::MatrixXd centered = subset_matrix.rowwise() - location.transpose();
    covariance = (centered.transpose() * centered) / static_cast<double>(h - 1);
    covariance = (covariance + covariance.transpose()) / 2.0;
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(covariance);
    if (solver.info() != Eigen::Success) return false;
    if (solver.eigenvalues()(0) < kMinEigenvalue) return false;
    return true;
}

McdCandidate run_cstep_trial(const Eigen::MatrixXd& points, const std::vector<int>& initial_subset) {
    int n = static_cast<int>(points.rows());
    int p = static_cast<int>(points.cols());
    int h = (n + p + 1) / 2;
    McdCandidate candidate;
    candidate.converged_iterations = 0;
    candidate.log_determinant = std::numeric_limits<double>::lowest();
    std::vector<int> current_subset = initial_subset;
    if (static_cast<int>(current_subset.size()) != h) {
        current_subset.resize(static_cast<std::size_t>(h));
    }
    for (int iter = 0; iter < kMaxCstepIterations; ++iter) {
        Eigen::VectorXd location;
        Eigen::MatrixXd covariance;
        if (!compute_mcd_statistics(points, current_subset, location, covariance)) {
            return candidate;
        }
        double prev_log_det = candidate.log_determinant;
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(covariance);
        candidate.log_determinant = 0.5 * solver.eigenvalues().array().log().sum();
        if (iter > 0 && std::abs(candidate.log_determinant - prev_log_det) < kCstepTolerance) {
            candidate.location = std::move(location);
            candidate.covariance = std::move(covariance);
            candidate.converged_iterations = iter + 1;
            return candidate;
        }
        Eigen::MatrixXd inv_cov;
        if (solver.info() != Eigen::Success) return candidate;
        Eigen::VectorXd inv_eigs = solver.eigenvalues().array().inverse();
        inv_cov = solver.eigenvectors() * inv_eigs.asDiagonal() * solver.eigenvectors().transpose();
        std::vector<std::pair<double, int>> distances;
        distances.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            Eigen::VectorXd diff = points.row(i).transpose() - location;
            double mahal_sq = diff.transpose() * inv_cov * diff;
            distances.emplace_back(mahal_sq, i);
        }
        std::sort(distances.begin(), distances.end());
        current_subset.clear();
        for (int i = 0; i < h; ++i) {
            current_subset.push_back(distances[static_cast<std::size_t>(i)].second);
        }
    }
    candidate.converged_iterations = kMaxCstepIterations;
    return candidate;
}
}

Result<FastMCDFit> fit_fastmcd(const Eigen::MatrixXd& points) {
    int n = static_cast<int>(points.rows());
    int p = static_cast<int>(points.cols());
    if (p < 1) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kInvalidArgument, "points has zero columns"));
    }
    if (n < p + 1) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kInvalidArgument, "fewer points than degrees of freedom + 1",
            "n=" + std::to_string(n) + ", p=" + std::to_string(p)));
    }
    int h = (n + p + 1) / 2;
    std::uint32_t seed = compute_data_seed(points);
    McdCandidate best_candidate;
    best_candidate.log_determinant = std::numeric_limits<double>::lowest();
    for (int trial = 0; trial < kNumInitialTrials; ++trial) {
        auto initial_subset = deterministic_h_subset(n, h, trial, seed);
        if (static_cast<int>(initial_subset.size()) < h) {
            continue;
        }
        auto candidate = run_cstep_trial(points, initial_subset);
        if (candidate.log_determinant > best_candidate.log_determinant) {
            best_candidate = std::move(candidate);
        }
    }
    if (best_candidate.log_determinant == std::numeric_limits<double>::lowest()) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kNumericFailure,
                                               "FastMCD: no valid candidate found across all trials"));
    }
    Eigen::VectorXd full_mean = points.colwise().mean();
    Eigen::MatrixXd full_centered = points.rowwise() - full_mean.transpose();
    Eigen::MatrixXd full_cov = (full_centered.transpose() * full_centered) / static_cast<double>(n - 1);
    full_cov = (full_cov + full_cov.transpose()) / 2.0;
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> h_solver(best_candidate.covariance);
    if (h_solver.info() != Eigen::Success) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kNumericFailure,
                                               "FastMCD: final covariance eigendecomposition failed"));
    }
    double h_log_det = 0.5 * h_solver.eigenvalues().array().log().sum();
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> full_solver(full_cov);
    if (full_solver.info() != Eigen::Success) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kNumericFailure,
                                               "FastMCD: full covariance eigendecomposition failed"));
    }
    double full_log_det = 0.5 * full_solver.eigenvalues().array().log().sum();
    double scale_factor = std::exp((h_log_det - full_log_det) / static_cast<double>(p));
    Eigen::MatrixXd reweighted_cov = full_cov * scale_factor;
    reweighted_cov = (reweighted_cov + reweighted_cov.transpose()) / 2.0;
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> final_solver(reweighted_cov);
    if (final_solver.info() != Eigen::Success) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kNumericFailure,
                                               "FastMCD: reweighted covariance eigendecomposition failed"));
    }
    if (final_solver.eigenvalues()(0) < kMinEigenvalue) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kNumericFailure,
                                               "FastMCD: reweighted covariance is (near-)singular"));
    }
    Eigen::VectorXd inv_eigs = final_solver.eigenvalues().array().inverse();
    Eigen::MatrixXd inv_cov =
        final_solver.eigenvectors() * inv_eigs.asDiagonal() * final_solver.eigenvectors().transpose();
    inv_cov = (inv_cov + inv_cov.transpose()) / 2.0;
    double max_mahal_sq = 0.0;
    for (int i = 0; i < n; ++i) {
        Eigen::VectorXd diff = points.row(i).transpose() - best_candidate.location;
        double mahal_sq = diff.transpose() * inv_cov * diff;
        max_mahal_sq = std::max(max_mahal_sq, mahal_sq);
    }
    return FastMCDFit{std::move(best_candidate.location), std::move(reweighted_cov), std::move(inv_cov), p, max_mahal_sq};
}

Result<FastMCDScore> score_fastmcd(const FastMCDFit& fit, const Eigen::VectorXd& point, double alpha) {
    if (point.size() != fit.location.size()) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kInvalidArgument, "point dimension does not match fit dimension",
            "point: " + std::to_string(point.size()) + ", fit: " + std::to_string(fit.location.size())));
    }
    if (!(alpha > 0.0 && alpha < 1.0)) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kInvalidArgument, "alpha must be in (0, 1)"));
    }
    Eigen::VectorXd diff = point - fit.location;
    double distance_squared = diff.transpose() * fit.inv_covariance * diff;
    if (distance_squared < 0.0) distance_squared = 0.0;
    double distance = std::sqrt(distance_squared);
    double p_value = 0.0, critical_distance = 0.0;
    try {
        boost::math::chi_squared_distribution<double> chi2(static_cast<double>(fit.degrees_of_freedom));
        p_value = boost::math::cdf(boost::math::complement(chi2, distance_squared));
        double critical_distance_squared = boost::math::quantile(chi2, 1.0 - alpha);
        critical_distance = std::sqrt(critical_distance_squared);
    } catch (const std::exception& e) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kNumericFailure,
                                               "chi-squared distribution evaluation failed", e.what()));
    }
    double depth = distance - critical_distance;
    bool inside = depth <= 0.0;
    return FastMCDScore{distance, critical_distance, p_value, depth, inside};
}
}