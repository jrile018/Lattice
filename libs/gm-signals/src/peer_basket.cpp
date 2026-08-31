#include <gm-signals/peer_basket.hpp>

#include <osqp/osqp.h>

#include <limits>
#include <vector>

namespace gm::signals {

namespace {

struct CscArrays {
    std::vector<OSQPInt> p, i;
    std::vector<OSQPFloat> x;
};

/// Upper-triangular part of a dense symmetric k x k matrix, in CSC -
/// what OSQP requires for the quadratic cost term P. Every entry is
/// included (no sparsity exploited) since k is small (the ADR default
/// is 8) and a fixed dense structural pattern per call is simpler and
/// safer than conditionally omitting near-zero entries.
CscArrays upper_triangular_csc(const Eigen::MatrixXd& m) {
    auto k = static_cast<int>(m.rows());
    CscArrays result;
    result.p.resize(static_cast<std::size_t>(k) + 1);
    result.p[0] = 0;
    for (int col = 0; col < k; ++col) {
        for (int row = 0; row <= col; ++row) {
            result.i.push_back(row);
            result.x.push_back(m(row, col));
        }
        result.p[static_cast<std::size_t>(col) + 1] = static_cast<OSQPInt>(result.i.size());
    }
    return result;
}

/// Constraint matrix A = [ones_row; I_k], (k+1) x k in CSC - row 0
/// encodes sum(w)=1 (via l[0]=u[0]=1), rows 1..k encode w_j >= 0 (via
/// l[j]=0, u[j]=+inf). Structural pattern depends only on k, not on any
/// data value.
CscArrays constraint_csc(int k) {
    CscArrays result;
    result.p.resize(static_cast<std::size_t>(k) + 1);
    result.p[0] = 0;
    for (int col = 0; col < k; ++col) {
        result.i.push_back(0); // the equality row
        result.x.push_back(1.0);
        result.i.push_back(col + 1); // this column's own non-negativity row
        result.x.push_back(1.0);
        result.p[static_cast<std::size_t>(col) + 1] = static_cast<OSQPInt>(result.i.size());
    }
    return result;
}

const char* status_name(OSQPInt status_val) {
    switch (status_val) {
        case OSQP_SOLVED: return "OSQP_SOLVED";
        case OSQP_PRIMAL_INFEASIBLE: return "OSQP_PRIMAL_INFEASIBLE";
        case OSQP_PRIMAL_INFEASIBLE_INACCURATE: return "OSQP_PRIMAL_INFEASIBLE_INACCURATE";
        case OSQP_MAX_ITER_REACHED: return "OSQP_MAX_ITER_REACHED";
        default: return "OSQP_OTHER";
    }
}

} // namespace

Result<Eigen::VectorXd> fit_peer_basket_weights(const Eigen::VectorXd& target_returns,
                                                 const Eigen::MatrixXd& neighbor_returns,
                                                 double ridge_lambda) {
    const Eigen::Index t = target_returns.size();
    const Eigen::Index k = neighbor_returns.cols();

    if (neighbor_returns.rows() != t) {
        return tl::unexpected(gm::Error::make(
            gm::ErrorCode::kInvalidArgument, "neighbor_returns row count must match target_returns length",
            "target: " + std::to_string(t) + ", neighbor rows: " + std::to_string(neighbor_returns.rows())));
    }
    if (k < 1) {
        return tl::unexpected(
            gm::Error::make(gm::ErrorCode::kInvalidArgument, "neighbor_returns has zero columns"));
    }
    if (ridge_lambda < 0.0) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kInvalidArgument, "ridge_lambda must be >= 0"));
    }

    const auto kk = static_cast<int>(k);

    Eigen::MatrixXd p_dense =
        2.0 * (neighbor_returns.transpose() * neighbor_returns + ridge_lambda * Eigen::MatrixXd::Identity(k, k));
    Eigen::VectorXd q_vec = -2.0 * (neighbor_returns.transpose() * target_returns);

    CscArrays p_arr = upper_triangular_csc(p_dense);
    CscArrays a_arr = constraint_csc(kk);

    OSQPCscMatrix* p_mat = OSQPCscMatrix_new(kk, kk, static_cast<OSQPInt>(p_arr.i.size()), p_arr.x.data(),
                                              p_arr.i.data(), p_arr.p.data());
    OSQPCscMatrix* a_mat = OSQPCscMatrix_new(kk + 1, kk, static_cast<OSQPInt>(a_arr.i.size()), a_arr.x.data(),
                                              a_arr.i.data(), a_arr.p.data());
    if (p_mat == nullptr || a_mat == nullptr) {
        if (p_mat) OSQPCscMatrix_free(p_mat);
        if (a_mat) OSQPCscMatrix_free(a_mat);
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kNumericFailure, "OSQPCscMatrix_new returned null"));
    }

    std::vector<OSQPFloat> l_vec(static_cast<std::size_t>(kk) + 1), u_vec(static_cast<std::size_t>(kk) + 1);
    l_vec[0] = 1.0;
    u_vec[0] = 1.0;
    for (int j = 0; j < kk; ++j) {
        l_vec[static_cast<std::size_t>(j) + 1] = 0.0;
        u_vec[static_cast<std::size_t>(j) + 1] = std::numeric_limits<OSQPFloat>::infinity();
    }

    OSQPSettings* settings = OSQPSettings_new();
    settings->verbose = 0;

    OSQPSolver* solver = nullptr;
    OSQPInt setup_flag = osqp_setup(&solver, p_mat, q_vec.data(), a_mat, l_vec.data(), u_vec.data(), kk + 1, kk,
                                     settings);

    // osqp_setup documented as copying problem data internally (the
    // solver must retain it across solve()/update_data_* calls, which
    // outlive this function's stack-local buffers) - safe to free our
    // CSC wrappers now regardless of setup's outcome. OSQPCscMatrix_free
    // only frees the wrapper struct here (owned=0 for user-backed
    // arrays per OSQPCscMatrix_new's contract), never our std::vectors.
    OSQPCscMatrix_free(p_mat);
    OSQPCscMatrix_free(a_mat);
    OSQPSettings_free(settings);

    if (setup_flag != 0 || solver == nullptr) {
        if (solver) osqp_cleanup(solver);
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kNumericFailure, "osqp_setup failed",
                                               "exitflag=" + std::to_string(setup_flag)));
    }

    OSQPInt solve_flag = osqp_solve(solver);
    OSQPInt status_val = solver->info->status_val;

    if (solve_flag != 0 || status_val != OSQP_SOLVED) {
        std::string detail = "solve exitflag=" + std::to_string(solve_flag) +
                              ", status=" + status_name(status_val);
        osqp_cleanup(solver);
        return tl::unexpected(
            gm::Error::make(gm::ErrorCode::kNumericFailure, "OSQP did not converge to a solution", detail));
    }

    Eigen::VectorXd weights(k);
    for (int j = 0; j < kk; ++j) weights(j) = solver->solution->x[j];
    osqp_cleanup(solver);

    // OSQP's convergence tolerance (default eps_abs/eps_rel ~1e-3) means
    // w >= 0 and sum(w) = 1 hold only approximately, not exactly -
    // clamp and renormalize so the returned basket is EXACTLY a valid
    // portfolio (non-negative, sums to 1), not merely close to one.
    weights = weights.cwiseMax(0.0);
    double sum = weights.sum();
    if (sum <= 0.0) {
        return tl::unexpected(gm::Error::make(
            gm::ErrorCode::kNumericFailure,
            "all basket weights were non-positive after clamping - degenerate solution"));
    }
    weights /= sum;

    return weights;
}

} // namespace gm::signals
