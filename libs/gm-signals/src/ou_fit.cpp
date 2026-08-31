#include <gm-signals/ou_fit.hpp>

#include <cmath>

namespace gm::signals {

Result<OuFit> fit_ou(const Eigen::VectorXd& s, double dt) {
    const Eigen::Index n = s.size();
    if (n < 3) {
        return tl::unexpected(
            gm::Error::make(gm::ErrorCode::kInvalidArgument, "fit_ou requires at least 3 points"));
    }
    if (!(dt > 0.0)) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kInvalidArgument, "dt must be > 0"));
    }

    // AR(1): s_t = c + phi*s_{t-1} + eps_t, over the n-1 consecutive
    // pairs. The MLE of (c, phi) for a Gaussian AR(1) coincides exactly
    // with the OLS regression of s_t on s_{t-1} - only the noise
    // variance estimator differs between MLE and the unbiased OLS
    // convention (MLE divides by n_pairs, not n_pairs - 2); "exact AR(1)
    // MLE mapping" per the ADR means using the MLE variance below, not
    // the unbiased one.
    const Eigen::Index n_pairs = n - 1;
    Eigen::VectorXd lagged = s.head(n_pairs);   // s_{t-1}, t=1..n-1
    Eigen::VectorXd current = s.tail(n_pairs);  // s_t,     t=1..n-1

    double lagged_mean = lagged.mean();
    double current_mean = current.mean();
    Eigen::VectorXd lagged_centered = lagged.array() - lagged_mean;
    Eigen::VectorXd current_centered = current.array() - current_mean;

    double lagged_var_sum = lagged_centered.squaredNorm();
    if (lagged_var_sum <= 0.0) {
        return tl::unexpected(gm::Error::make(
            gm::ErrorCode::kInvalidArgument, "spread series is constant - AR(1) slope is undefined"));
    }

    double phi = lagged_centered.dot(current_centered) / lagged_var_sum;
    double c = current_mean - phi * lagged_mean;

    if (!(phi > 0.0 && phi < 1.0)) {
        return tl::unexpected(gm::Error::make(
            gm::ErrorCode::kNumericFailure,
            "AR(1) coefficient outside (0,1) - series is not a stationary mean-reverting process "
            "(phi<=0: oscillating/degenerate; phi>=1: non-stationary, no finite half-life)",
            "phi=" + std::to_string(phi)));
    }

    Eigen::VectorXd residuals = current.array() - (c + phi * lagged.array());
    double sigma_eps_sq = residuals.squaredNorm() / static_cast<double>(n_pairs); // MLE: divide by n_pairs

    double theta = -std::log(phi) / dt;
    double mu = c / (1.0 - phi);
    // sigma^2 = sigma_eps^2 * 2*theta / (1 - phi^2), from matching the
    // OU process's exact discretization variance
    // Var(eps) = sigma^2/(2*theta) * (1 - exp(-2*theta*dt)) = sigma^2/(2*theta)*(1-phi^2)
    // (since phi = exp(-theta*dt) by construction, phi^2 = exp(-2*theta*dt)).
    double sigma_sq = sigma_eps_sq * 2.0 * theta / (1.0 - phi * phi);
    double sigma = std::sqrt(std::max(sigma_sq, 0.0));
    double half_life = std::log(2.0) / theta;

    return OuFit{theta, mu, sigma, half_life};
}

Result<double> ou_zscore(const OuFit& fit, double s_t) {
    if (!(fit.theta > 0.0)) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kInvalidArgument, "fit.theta must be > 0"));
    }
    if (!(fit.sigma > 0.0)) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kInvalidArgument, "fit.sigma must be > 0"));
    }
    // Stationary standard deviation of the OU process: sqrt(sigma^2 / (2*theta)).
    double stationary_std = fit.sigma / std::sqrt(2.0 * fit.theta);
    return (s_t - fit.mu) / stationary_std;
}

} // namespace gm::signals
