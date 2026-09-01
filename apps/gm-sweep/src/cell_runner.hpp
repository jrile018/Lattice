#pragma once

#include <gm-core/error.hpp>
#include <gm-sweep/parameter_grid.hpp>

#include <filesystem>
#include <string>

namespace gm::sweep::execution {

namespace fs = std::filesystem;

// Result of executing a single sweep cell (only signals + backtest, not full pipeline).
struct CellResult {
    int cell_id;
    bool success;
    double sharpe_ratio;      // Only valid if success == true
    std::string error_message; // Only set if success == false
};

// Execute a single cell: override config, symlink upstream artifacts, run gm-signals and gm-backtest.
// upstream_root: directory containing gm-universe, gm-ingest, etc. (typically the m1-full run)
// base_config_path: path to master config
// sweep_root: parent directory where cell_<id>/ will be created
// signals_exe, backtest_exe: paths to gm-signals and gm-backtest executables
// bin_dir: directory containing stage executables (for data dependencies)
// run_id: numeric run identifier
[[nodiscard]] CellResult execute_cell(
    const SweepCell& cell,
    const fs::path& upstream_root,
    const fs::path& base_config_path,
    const fs::path& sweep_root,
    const fs::path& signals_exe,
    const fs::path& backtest_exe,
    const fs::path& bin_dir,
    int run_id);

} // namespace gm::sweep::execution
