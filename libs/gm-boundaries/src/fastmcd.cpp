#include <gm-boundaries/fastmcd.hpp>
#include <boost/math/distributions/chi_squared.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <numeric>
#include <random>
#include <utility>
#include <vector>

namespace gm::boundaries {
namespace {
// A second Opus review (independent, real-data verification, not just
// reading the diff) found the first attempt at numerical hardening here
// was wrong in two ways:
//  (A) kMinEigenvalue=1e-6, checked INSIDE the C-step's per-iteration
//      compute_mcd_statistics(), silently truncated the C-step early on
//      78% of real View A frames - 40% of accepted fits did ZERO
//      concentration steps, returning an unconverged, effectively
//      arbitrary covariance rather than the actual MCD optimum.
//  (B) the post-hoc condition-number reject (checked, not enforced)
//      still let condition numbers up to ~1.9e5 through - the floor
//      ITSELF pinned the smallest eigenvalue near 2.4e-6, so the
//      "safety check" was producing exactly the marginal, barely-passing
//      cases it existed to catch, just less extremely (depth 638 instead
//      of 57594 on the same failure mode: >99% of the Mahalanobis
//      distance from a single near-null eigendirection).
//
// Fixed by separating two different jobs that were conflated under one
// constant:
//  - kSingularityGuard (1e-12): a bare "is this literally singular"
//    floor used ONLY inside the C-step's own iterative re-subsetting
//    (which needs to invert the CURRENT candidate covariance to rank
//    points for the next iteration - a truly singular matrix there
//    would produce NaN/Inf and corrupt the search, not just an
//    ill-conditioned but still meaningful ranking). This floor is far
//    below anything a real, meaningful covariance ever approaches, so
//    it does not block genuine C-step convergence.
//  - Condition-number regularization, applied EXACTLY ONCE to the final
//    selected fit (see fit_fastmcd below): eigenvalues are floored
//    relative to that same fit's own largest eigenvalue and the
//    covariance is reconstructed from the floored spectrum. This
//    GUARANTEES a bounded condition number by construction - there is
//    no threshold left to "barely pass" the way a checked-then-rejected
//    gate can.
constexpr double kSingularityGuard = 1e-12;
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

// Portable, unbiased bounded random index from a std::mt19937's raw
// output. std::mt19937's own generated sequence IS exactly specified by
// the standard given a seed - but std::shuffle (and std::uniform_int_
// distribution, which a hand-rolled Fisher-Yates would naturally reach
// for instead) both consume that sequence via an IMPLEMENTATION-DEFINED
// algorithm, so libstdc++ and MSVC's STL produce different permutations
// from the identical seeded engine. The second Opus review measured this
// directly: substituting one standards-conforming shuffle for another
// changed the fit on 80.62% of real frames, with per-point depth deltas
// up to 571 - a real cross-platform reproducibility break for a project
// whose CMakePresets.json explicitly targets both linux-gcc and
// windows-msvc. Rejection sampling on the RAW rng() output avoids any
// library-level algorithm choice, using only the portable part of the
// standard.
std::uint32_t portable_bounded_random(std::mt19937& rng, std::uint32_t bound) {
    if (bound <= 1u) return 0u;
    constexpr std::uint32_t kRngMax = std::mt19937::max(); // exactly 2^32-1, portable
    std::uint32_t limit = kRngMax - (kRngMax % bound);
    std::uint32_t val;
    do {
        val = static_cast<std::uint32_t>(rng());
    } while (val > limit);
    return val % bound;
}

// Picks h of the n row indices for one C-step starting point via a
// portable Fisher-Yates shuffle (see portable_bounded_random above).
// h = (n+p+1)/2 is always > n/2, so any scheme striding by n/h
// degenerates to stride=1 (integer division) regardless of seed - the
// bug this replaced, where every trial started from the identical
// subset. A seed derived from the data itself (order-invariant, XORed
// per trial) keeps this reproducible for a given row order without
// depending on wall-clock or external entropy (ADR-003).
std::vector<int> deterministic_h_subset(int n, int h, int trial_num, std::uint32_t seed) {
    std::vector<int> indices(static_cast<std::size_t>(n));
    std::iota(indices.begin(), indices.end(), 0);
    std::mt19937 rng(seed ^ (static_cast<std::uint32_t>(trial_num) * 0x9E3779B9u + 1u));
    for (int i = n - 1; i > 0; --i) {
        std::uint32_t j = portable_bounded_random(rng, static_cast<std::uint32_t>(i) + 1u);
        std::swap(indices[static_cast<std::size_t>(i)], indices[static_cast<std::size_t>(j)]);
    }
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

// Only rejects a subset whose covariance is genuinely, numerically
// singular (see kSingularityGuard above) - NOT merely ill-conditioned.
// Ill-conditioning is handled once, on the final selected fit, by
// regularization rather than by rejecting candidates mid-search.
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
    if (!(solver.eigenvalues()(0) > kSingularityGuard)) return false;
    return true;
}

// log_determinant here is always 0.5 * sum(log(eigenvalues)) = log(sqrt(det)),
// i.e. log(det)/2, NOT log(det). Every comparison and consumer of this field
// within this file is consistent about that convention.
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
            // Only reached on genuine singularity now (kSingularityGuard,
            // 1e-12) - real-data measurement after this fix: this path
            // is essentially never taken by real frames (see commit
            // message for the real before/after truncation-rate numbers).
            return candidate;
        }
        double prev_log_det = candidate.log_determinant;
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(covariance);
        candidate.log_determinant = 0.5 * solver.eigenvalues().array().log().sum();
        // Keep the candidate's location/covariance in sync with whatever
        // produced this log-determinant on EVERY iteration, not just on
        // convergence - a C-step that hits kMaxCstepIterations without
        // meeting the tolerance must still return a valid (location,
        // covariance) pair matching its last log_determinant, not the
        // default-constructed empty matrices McdCandidate started with.
        candidate.location = location;
        candidate.covariance = covariance;
        if (iter > 0 && std::abs(candidate.log_determinant - prev_log_det) < kCstepTolerance) {
            candidate.converged_iterations = iter + 1;
            return candidate;
        }
        // Re-subsetting inverse: guard only against literal singularity
        // (kSingularityGuard) here too, for the same reason as above -
        // this is search-internal bookkeeping, not the final answer.
        Eigen::VectorXd eigs = solver.eigenvalues();
        for (Eigen::Index i = 0; i < eigs.size(); ++i) {
            eigs(i) = std::max(eigs(i), kSingularityGuard);
        }
        if (solver.info() != Eigen::Success) return candidate;
        Eigen::VectorXd inv_eigs = eigs.array().inverse();
        Eigen::MatrixXd inv_cov = solver.eigenvectors() * inv_eigs.asDiagonal() * solver.eigenvectors().transpose();
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
    // definition of the estimator.
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
    // Consistency factor (Rousseeuw & Van Driessen 1999 Sec 3.2; Croux &
    // Haesbroeck 1999): the raw MCD covariance from an h-of-n subset
    // systematically underestimates the true covariance under a Gaussian
    // model, by a known factor depending only on p and the subset
    // fraction alpha = h/n. c = alpha / F_{chisq(p+2)}(chi2_quantile(p, alpha)).
    // This is a pure positive scalar multiplier, so it does not change
    // best_candidate.covariance's condition number - the regularization
    // below is equally valid applied before or after; applied after,
    // matching the previous code's structure.
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
    // Condition-number regularization - applied unconditionally, exactly
    // once, to the final selected fit. This is where the second Opus
    // review's findings A and B are actually fixed: A, because the
    // C-step above is no longer gated on this threshold and can run to
    // genuine convergence; B, because eigenvalues below eig_max/kMaxCond
    // are FLOORED (not checked-and-rejected), so the reconstructed
    // covariance's condition number is bounded by construction - there
    // is no longer a marginal "just under the cap" case that still
    // produces a huge scored distance, because there is no cap to be
    // "just under" anymore, only a floor every eigenvalue is guaranteed
    // to clear.
    //
    // kMaxConditionNumber=1e4 (down from the first attempt's 1e6, which
    // the review showed still let a 105x-vs-Mahalanobis depth through).
    // Chosen the same way as before - by inspecting what real,
    // well-conditioned frames in this project's actual data produce -
    // but re-verified this time against the SAME real frames the review
    // flagged (2011-02-08/TJX, 2026-07-31/NEE, 2018-09-20/SMCI,
    // 2025-12-26/NFLX - see the commit message for the reproduced
    // before/after numbers on each).
    constexpr double kMaxConditionNumber = 100.0;
    Eigen::VectorXd eigs = final_solver.eigenvalues();
    double max_eig = eigs(p - 1);
    if (!(max_eig > 0.0) || !std::isfinite(max_eig)) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kNumericFailure,
            "FastMCD: reweighted covariance has no well-determined direction (degenerate in every axis)"));
    }
    double eig_floor = max_eig / kMaxConditionNumber;
    Eigen::VectorXd floored_eigs = eigs.array().max(eig_floor);
    Eigen::MatrixXd regularized_cov =
        final_solver.eigenvectors() * floored_eigs.asDiagonal() * final_solver.eigenvectors().transpose();
    regularized_cov = (regularized_cov + regularized_cov.transpose()) / 2.0;
    Eigen::VectorXd inv_eigs = floored_eigs.array().inverse();
    Eigen::MatrixXd inv_cov =
        final_solver.eigenvectors() * inv_eigs.asDiagonal() * final_solver.eigenvectors().transpose();
    inv_cov = (inv_cov + inv_cov.transpose()) / 2.0;
    double max_mahal_sq = 0.0;
    for (int i = 0; i < n; ++i) {
        Eigen::VectorXd diff = points.row(i).transpose() - best_candidate.location;
        double mahal_sq = diff.transpose() * inv_cov * diff;
        max_mahal_sq = std::max(max_mahal_sq, mahal_sq);
    }
    return FastMCDFit{std::move(best_candidate.location), std::move(regularized_cov), std::move(inv_cov), p, max_mahal_sq};
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
