#pragma once

#include <gm-core/error.hpp>
#include <gm-sweep/parameter_grid.hpp>

#include <filesystem>
#include <string>

namespace gm::sweep::execution {

namespace fs = std::filesystem;

struct CellResult {
    int cell_id;
    bool success;
    double sharpe_ratio;
    std::string error_message;
};

[[nodiscard]] CellResult execute_cell(
    const SweepCell& cell,
    const fs::path& upstream_root,
    const fs::path& base_config_path,
    const fs::path& sweep_root,
    const fs::path& signals_exe,
    const fs::path& backtest_exe,
    int run_id);

} // namespace gm::sweep::execution
