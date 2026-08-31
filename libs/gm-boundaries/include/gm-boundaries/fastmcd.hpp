#pragma once

// Robust Minimum Covariance Determinant (FastMCD) estimator (ADR-011 phase 2)
// 
// Implements Rousseeuw & Van Driessen (1999): "A fast algorithm for the
// minimum covariance determinant estimator" - finds the h-subset of n points
// (where h = ceil((n+p+1)/2)) whose covariance matrix has the smallest
// determinant, yielding a robust location/covariance estimate resistant to
// ~50% contamination.
//
// DETERMINISM RESOLUTION (ADR-003 conflict with classical algorithm):
// The published FastMCD uses random initial h-subsets in the C-step.
// To maintain bit-reproducibility (ADR-003: "no wall-clock, no unseeded RNG
// in production paths"), we:
// 1. Derive a fixed seed from a canonical (sorted) representation of the data
//    via a deterministic hash function, ensuring identical input gives
//    identical seed regardless of input order.
// 2. Use this seed to generate multiple deterministic initial subsets via
//    systematic sampling (stride-based selection) rather than random sampling.
// 3. Run the C-step concentration procedure identically on each subset.
// 4. Select the best result (smallest determinant) across trials.
// 
// This preserves the algorithm's robustness (multiple starting points, hill
// climbing to a local optimum, selecting the global best found) while
// guaranteeing reproducibility. The same data in any order yields the same
// FastMCD estimate.

#include <gm-core/error.hpp>

#include <Eigen/Dense>

namespace gm::boundaries {

struct FastMCDFit {
    Eigen::VectorXd location;       // p, the robust center estimate
    Eigen::MatrixXd covariance;     // p x p, the robust covariance estimate
    Eigen::MatrixXd inv_covariance; // precomputed inverse for fast scoring
    int degrees_of_freedom;         // p
    double mahalanobis_squared_at_worst_h_point;  // diagnostic: the max M-distance
};

struct FastMCDScore {
    double distance;      // Mahalanobis distance (not squared) in robust space
    double critical_distance; // the chi-squared-quantile distance at `alpha`
    double p_value;        // chi-squared survival probability at distance^2, df=p
    double depth;           // distance - critical_distance: negative=inside, positive=outside
    bool inside;
};

/// `points` is N x k (N >= k+1 required; typically k is 2-5 for boundary scoring).
/// Every dimension's variance must be nonzero.
/// 
/// Returns the robust location and covariance estimate, along with the inverse
/// covariance precomputed for efficient scoring.
[[nodiscard]] Result<FastMCDFit> fit_fastmcd(const Eigen::MatrixXd& points);

/// Score a single point under the robust fit.
/// `alpha` is the boundary's exceedance probability (default 0.05).
[[nodiscard]] Result<FastMCDScore> score_fastmcd(const FastMCDFit& fit,
                                                    const Eigen::VectorXd& point,
                                                    double alpha = 0.05);

} // namespace gm::boundaries
