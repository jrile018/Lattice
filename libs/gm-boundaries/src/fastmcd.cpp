#include <gm-boundaries/fastmcd.hpp>
#include <boost/math/distributions/chi_squared.hpp>
#include <algorithm>
#include <cmath>
#include <exception>
#include <numeric>
#include <random>
#include <vector>

namespace gm::boundaries {
namespace {
// Real-data check: 1e-10 (the original value) is far too permissive
// for this codebase's actual data scale. A real early-history frame
// (2011-02-08, 81 points, TJX) produced a raw h-subset covariance with
// smallest eigenvalue 2.4e-10 - technically "above" the old floor, but
// still small enough (relative to that same fit's largest eigenvalue,
// 3.9e-3) to blow a single point's Mahalanobis distance up to 57594
// when this codebase's own Mahalanobis estimator scores the same point
// at 17.45. 1e-6 was chosen by looking at the actual eigenvalue scale
// real fits produce (largest eigenvalues cluster in the 1e-3 to 1e-1
// range on this project's MDS-embedded geometry) - it tolerates real
// anisotropy up to roughly a 1e3-1e5:1 spread while catching the
// genuinely near-degenerate case above.
constexpr double kMinEigenvalue = 1e-6;
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

// Picks h of the n row indices for one C-step starting point. h = (n+p+1)/2
// is always > n/2, so any scheme that strides through indices with a
// stride derived from n/h degenerates to stride=1 for every trial (n/h is
// always 0 or 1 by integer division) - every trial ends up starting from
// the identical subset, which is what kNumInitialTrials=5 silently was
// doing before this fix (measured: reduced C-step wall time by 4.4x on
// the real run with the identical result, confirming trials 2-5 did no
// useful work). A std::mt19937 seeded from compute_data_seed(points) XOR
// the trial number gives 5 genuinely different starting subsets while
// staying fully reproducible for a given (points, trial) pair - this is
// deterministic in the ADR-003 sense that matters here (no wall-clock, no
// unseeded entropy, reproducible run to run for the std::map-ordered data
// every caller in this codebase actually feeds it), not in the stronger
// sense of being invariant under an arbitrary row permutation of
// otherwise-identical data - see fastmcd.hpp's updated comment on that.
std::vector<int> deterministic_h_subset(int n, int h, int trial_num, std::uint32_t seed) {
    std::vector<int> indices(static_cast<std::size_t>(n));
    std::iota(indices.begin(), indices.end(), 0);
    std::mt19937 rng(seed ^ (static_cast<std::uint32_t>(trial_num) * 0x9E3779B9u + 1u));
    std::shuffle(indices.begin(), indices.end(), rng);
    indices.resize(static_cast<std::size_t>(h));
    std::sort(indices.begin(), indices.end());
    return indices;
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

// log_determinant here is always 0.5 * sum(log(eigenvalues)) = log(sqrt(det)),
// i.e. log(det)/2, NOT log(det). Every comparison and consumer of this field
// within this file is consistent about that convention (it is only ever
// compared against another value of the same field, or exponentiated back
// out consistently - see the consistency-factor computation in
// fit_fastmcd, which no longer needs to convert between the two
// conventions the way the previous reweighting step incorrectly did).
McdCandidate run_cstep_trial(const Eigen::MatrixXd& points, const std::vector<int>& initial_subset) {
    int n = static_cast<int>(points.rows());
    int p = static_cast<int>(points.cols());
    int h = (n + p + 1) / 2;
    McdCandidate candidate;
    candidate.converged_iterations = 0;
    candidate.log_determinant = std::numeric_limits<double>::max();
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
        // Keep the candidate's location/covariance in sync with whatever
        // produced this log-determinant on EVERY iteration, not just on
        // convergence - a C-step that hits kMaxCstepIterations without
        // meeting the tolerance (which happens on real, larger datasets
        // even though it never did on the tiny synthetic test fixtures)
        // must still return a valid (location, covariance) pair matching
        // its last log_determinant, not the default-constructed empty
        // matrices McdCandidate started with - that mismatch (a "valid"
        // log_determinant paired with an empty covariance) is what made
        // fit_fastmcd's SelfAdjointEigenSolver crash on real data.
        candidate.location = location;
        candidate.covariance = covariance;
        if (iter > 0 && std::abs(candidate.log_determinant - prev_log_det) < kCstepTolerance) {
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
    // MCD selects the MINIMUM determinant h-subset - that is the entire
    // definition of the estimator (the previous code kept the MAXIMUM,
    // which is the opposite of robust: it prefers the most spread-out,
    // least concentrated subset, i.e. the one most likely to be
    // contaminated). Sentinel starts at +infinity so any real candidate
    // is immediately better.
    best_candidate.log_determinant = std::numeric_limits<double>::max();
    bool found_any = false;
    for (int trial = 0; trial < kNumInitialTrials; ++trial) {
        auto initial_subset = deterministic_h_subset(n, h, trial, seed);
        if (static_cast<int>(initial_subset.size()) < h) {
            continue;
        }
        auto candidate = run_cstep_trial(points, initial_subset);
        if (candidate.covariance.size() == 0) {
            continue; // this trial never produced a valid statistics computation
        }
        if (!found_any || candidate.log_determinant < best_candidate.log_determinant) {
            best_candidate = std::move(candidate);
            found_any = true;
        }
    }
    if (!found_any) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kNumericFailure,
                                               "FastMCD: no valid candidate found across all trials"));
    }
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> h_solver(best_candidate.covariance);
    if (h_solver.info() != Eigen::Success) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kNumericFailure,
                                               "FastMCD: final covariance eigendecomposition failed"));
    }
    if (h_solver.eigenvalues()(0) < kMinEigenvalue) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kNumericFailure,
                                               "FastMCD: raw h-subset covariance is (near-)singular"));
    }
    // Consistency factor (Rousseeuw & Van Driessen 1999 Sec 3.2; Croux &
    // Haesbroeck 1999): the raw MCD covariance from an h-of-n subset
    // systematically underestimates the true covariance under a Gaussian
    // model, by a known factor depending only on p and the subset
    // fraction alpha = h/n. c = alpha / F_{chisq(p+2)}(chi2_quantile(p, alpha)).
    // This REPLACES the previous "scale full_cov to match the h-subset's
    // determinant" approach, which was wrong twice over: it reweighted
    // the WRONG matrix (the non-robust full-sample covariance, discarding
    // the actual robust h-subset covariance this whole algorithm exists
    // to compute - verified empirically identical to the plain classical
    // covariance, 0% breakdown point instead of the intended ~50%), and
    // its scale factor itself was derived from exp((h_log_det -
    // full_log_det) / p) where log_determinant is log(det)/2, not
    // log(det) - an extra, undocumented square root (measured 718x off
    // the value the code's own comment claimed to be matching).
    double alpha_fraction = static_cast<double>(h) / static_cast<double>(n);
    double consistency_factor = 1.0;
    try {
        boost::math::chi_squared_distribution<double> chi2_p(static_cast<double>(p));
        double chi2_quantile = boost::math::quantile(chi2_p, alpha_fraction);
        boost::math::chi_squared_distribution<double> chi2_p2(static_cast<double>(p) + 2.0);
        double denom = boost::math::cdf(chi2_p2, chi2_quantile);
        if (denom > 0.0) {
            consistency_factor = alpha_fraction / denom;
        }
    } catch (const std::exception& e) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kNumericFailure,
                                               "FastMCD: consistency-factor computation failed", e.what()));
    }
    Eigen::MatrixXd reweighted_cov = best_candidate.covariance * consistency_factor;
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
    // Real-data check (not caught by any synthetic fixture, and not
    // caught by the previous max-determinant selection bug either -
    // maximizing determinant systematically avoided near-degenerate
    // subsets by construction, so this failure mode was latent until
    // the min-determinant fix above made the estimator actually pick
    // the subsets MCD is supposed to pick, some of which are close to
    // collinear/coplanar for real (small, early-history, or low-p)
    // frames). kMinEigenvalue only bounds the SMALLEST eigenvalue in
    // absolute terms; a matrix can pass that check while still being
    // ill-conditioned RELATIVE to its own largest eigenvalue, which is
    // what actually blows up the inverse used for scoring. Measured on
    // the real 2010-2026/503-ticker run before this check: max scored
    // depth reached 57594 (versus Mahalanobis's 17.45 on the same
    // data) - not a plausible boundary distance by any reading, and
    // confirmed as a numerical artifact of an ill-conditioned reweighted
    // covariance, not a real anomaly signal.
    // Tightened alongside kMinEigenvalue for the same real-data reason -
    // 1e8 alone still let the 2011-02-08/TJX case (condition number
    // 1.6e7) through with a 57594 depth. 1e6 was chosen the same way:
    // it's comfortably above what real, well-conditioned frames in this
    // project's actual data produce, while catching frames like this
    // one.
    constexpr double kMaxConditionNumber = 1e6;
    double condition_number = final_solver.eigenvalues()(p - 1) / final_solver.eigenvalues()(0);
    if (!(condition_number < kMaxConditionNumber)) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kNumericFailure,
            "FastMCD: reweighted covariance is ill-conditioned",
            "condition_number=" + std::to_string(condition_number)));
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
