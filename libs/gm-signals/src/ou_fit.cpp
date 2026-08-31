#include <gm-signals/ou_fit.hpp>

#include <cmath>

namespace gm::signals {

namespace {
// mu = c / (1 - phi): as phi -> 1 (near-unit-root), this division
// becomes numerically explosive - found the hard way on the real
// 16-year pipeline (ADR Sec3 principle 1: found via a real run, not a
// unit test that happened to hit it). A concrete case: a 60-day window
// fit phi=0.9999963832 (1-phi = 3.617e-6, half_life = 191,648 days).
// sigma itself stays well-behaved there (2*theta/(1-phi^2) -> 1 in the
// mathematical limit as phi->1, confirmed by direct computation: it
// evaluated to 1.0000036, not collapsed toward zero) - the actual
// culprit is mu: c is an ordinary-magnitude regression intercept with
// no reason to be precisely proportional to (1-phi), so dividing it by
// a near-zero denominator sends mu to an arbitrary, wildly wrong
// magnitude, which then dominates (s-mu) and produces an absurd
// z-score (observed: z=-222.8 for a spread that was not remotely 222
// standard deviations from anything real). This is the textbook
// near-unit-root mean-identification breakdown for AR(1)/OU
// estimation, not a sign of "genuinely low noise" - a real risk this
// project's own ADR-013 reversion study would otherwise silently
// inherit as corrupted extreme-depth-bucket statistics.
//
// The threshold is chosen with a wide margin either side of the two
// concrete cases observed on the real dataset: the pathological one
// above (1-phi=3.6e-6) and an adjacent, plausible slow-but-real fit on
// the same ticker just days earlier (1-phi=3.77e-3, half_life=184
// days) - 1e-4 sits three orders of magnitude from the former and
// ~38x below the latter, rejecting the former while comfortably
// admitting the latter. Equivalently, this caps half_life at roughly
// ln(2)/-ln(1-1e-4) =~ 6931 days (~19 years) - generous enough not to
// reject genuinely slow mean reversion, tight enough to catch the
// numerically unstable regime.
constexpr double kMinOneMinusPhi = 1e-4;
} // namespace

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
    if (1.0 - phi < kMinOneMinusPhi) {
        return tl::unexpected(gm::Error::make(
            gm::ErrorCode::kNumericFailure,
            "AR(1) coefficient too close to 1 (near-unit-root) - mu=c/(1-phi) is numerically "
            "unreliable in this regime even though phi is nominally inside (0,1)",
            "phi=" + std::to_string(phi) + ", 1-phi=" + std::to_string(1.0 - phi)));
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
