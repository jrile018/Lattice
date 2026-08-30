#pragma once

// Kernel density level-set boundary (ADR-011): a Gaussian KDE with a
// diagonal (per-dimension) bandwidth via Scott's rule, and a level set
// - the density threshold containing (1-alpha) of the training mass -
// as the "surface." Represents the lumpy, non-convex normal regions a
// single ellipsoid (mahalanobis.hpp) cannot.

#include <gm-core/error.hpp>

#include <Eigen/Dense>

namespace gm::boundaries {

struct KdeFit {
    Eigen::MatrixXd training_points; // N x k, kept for scoring (KDE is non-parametric: the "model"
                                      // is the training set itself plus the bandwidth)
    Eigen::VectorXd bandwidth;       // k, per-dimension Gaussian kernel bandwidth (Scott's rule)
    double level;                    // density threshold for "inside" at the fit's alpha
    double alpha;                    // the exceedance probability this level was fit at
};

struct KdeScore {
    double density;
    double log_density;
    double depth; // log(level) - log(density): negative=inside (denser than the level), positive=outside
    bool inside;
};

/// `points` is N x k (N >= 2). `alpha` is the boundary's exceedance
/// probability: `level` is set so that a fraction (1-alpha) of the
/// TRAINING points themselves have density >= level (evaluated
/// in-sample, not leave-one-out - a documented simplification, not a
/// silent one; leave-one-out would reduce self-bias further but adds
/// O(N^2) cost this phase-1 estimator does not need).
[[nodiscard]] Result<KdeFit> fit_kde(const Eigen::MatrixXd& points, double alpha = 0.05);

[[nodiscard]] Result<double> kde_density(const KdeFit& fit, const Eigen::VectorXd& point);

[[nodiscard]] Result<KdeScore> score_kde(const KdeFit& fit, const Eigen::VectorXd& point);

} // namespace gm::boundaries
