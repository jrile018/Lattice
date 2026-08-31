#include <osqp/osqp.h>
#include <iostream>
#include <vector>

int main() {
    // Reproduce the k=1 case directly against the raw OSQP API, bypassing
    // gm::signals entirely, to see exactly what the solver reports.
    OSQPFloat p_x[1] = {45.0};
    OSQPInt p_i[1] = {0};
    OSQPInt p_p[2] = {0, 1};
    OSQPCscMatrix* P = OSQPCscMatrix_new(1, 1, 1, p_x, p_i, p_p);

    OSQPFloat q[1] = {-35.0};

    OSQPFloat a_x[2] = {1.0, 1.0};
    OSQPInt a_i[2] = {0, 1};
    OSQPInt a_p[2] = {0, 2};
    OSQPCscMatrix* A = OSQPCscMatrix_new(2, 1, 2, a_x, a_i, a_p);

    OSQPFloat l[2] = {1.0, 0.0};
    OSQPFloat u[2] = {1.0, 1e30};

    OSQPSettings* settings = OSQPSettings_new();
    std::cout << "default verbose=" << settings->verbose << " max_iter=" << settings->max_iter
              << " eps_abs=" << settings->eps_abs << " eps_rel=" << settings->eps_rel
              << " rho=" << settings->rho << " sigma=" << settings->sigma
              << " alpha=" << settings->alpha << " scaling=" << settings->scaling << "\n";
    settings->verbose = 1; // let OSQP print its own iteration log this time

    OSQPSolver* solver = nullptr;
    OSQPInt setup_flag = osqp_setup(&solver, P, q, A, l, u, 2, 1, settings);
    std::cout << "setup_flag=" << setup_flag << " solver=" << solver << "\n";
    if (solver == nullptr) return 1;

    OSQPInt solve_flag = osqp_solve(solver);
    std::cout << "solve_flag=" << solve_flag << " status_val=" << solver->info->status_val
              << " iter=" << solver->info->iter << "\n";
    if (solver->solution && solver->solution->x) {
        std::cout << "x[0]=" << solver->solution->x[0] << "\n";
    }

    osqp_cleanup(solver);
    OSQPCscMatrix_free(P);
    OSQPCscMatrix_free(A);
    OSQPSettings_free(settings);
    return 0;
}
