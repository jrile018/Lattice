#include <gm-backtest/deflated_sharpe.hpp>

#include <boost/math/distributions/normal.hpp>

#include <cmath>
#include <exception>

namespace gm::backtest {

namespace {
constexpr double kEulerMascheroni = 0.5772156649015329; // gamma in the paper's Eq. 1/5
} // namespace

Result<double> expected_max_sharpe(double trial_sharpe_variance, int n_trials) {
    if (!(trial_sharpe_variance >= 0.0)) {
        return tl::unexpected(
            gm::Error::make(gm::ErrorCode::kInvalidArgument, "trial_sharpe_variance must be >= 0"));
    }
    if (n_trials < 1) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kInvalidArgument, "n_trials must be >= 1"));
    }
    if (n_trials == 1) {
        // Paper's own stated special case: with a single trial there is
        // no selection to correct for.
        return 0.0;
    }

    try {
        boost::math::normal_distribution<double> standard_normal(0.0, 1.0);
        double n = static_cast<double>(n_trials);
        double term1 = boost::math::quantile(standard_normal, 1.0 - 1.0 / n);
        double term2 = boost::math::quantile(standard_normal, 1.0 - 1.0 / (n * std::exp(1.0)));
        double bracket = (1.0 - kEulerMascheroni) * term1 + kEulerMascheroni * term2;
        return std::sqrt(trial_sharpe_variance) * bracket;
    } catch (const std::exception& e) {
        return tl::unexpected(
            gm::Error::make(gm::ErrorCode::kNumericFailure, "expected_max_sharpe evaluation failed", e.what()));
    }
}

Result<double> deflated_sharpe_ratio(double observed_sharpe, double sr0, int t_observations, double skewness,
                                      double kurtosis) {
    if (t_observations < 2) {
        return tl::unexpected(
            gm::Error::make(gm::ErrorCode::kInvalidArgument, "t_observations must be >= 2"));
    }

    double t = static_cast<double>(t_observations);
    double sr = observed_sharpe;
    // Paper Eq. 2's denominator, algebraically simplified from its
    // presentation elsewhere (Quantdare's exposition of the same
    // formula writes it as 1 + 0.5*sr^2 - skew*sr + (kurtosis-3)/4*sr^2,
    // which combines to exactly this - verified by hand during design,
    // both forms checked against the paper's own worked example below).
    double variance_term = 1.0 - skewness * sr + (kurtosis - 1.0) / 4.0 * sr * sr;
    if (!(variance_term > 0.0)) {
        return tl::unexpected(gm::Error::make(
            gm::ErrorCode::kNumericFailure,
            "DSR denominator is non-positive - skewness/kurtosis/Sharpe combination is degenerate",
            "variance_term=" + std::to_string(variance_term)));
    }

    try {
        boost::math::normal_distribution<double> standard_normal(0.0, 1.0);
        double z_arg = (sr - sr0) * std::sqrt(t - 1.0) / std::sqrt(variance_term);
        return boost::math::cdf(standard_normal, z_arg);
    } catch (const std::exception& e) {
        return tl::unexpected(
            gm::Error::make(gm::ErrorCode::kNumericFailure, "deflated_sharpe_ratio evaluation failed", e.what()));
    }
}

} // namespace gm::backtest
