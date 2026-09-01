#pragma once

#include <gm-core/error.hpp>
#include <gm-sweep/parameter_grid.hpp>

#include <filesystem>
#include <toml++/toml.hpp>

namespace gm::sweep::execution {

namespace fs = std::filesystem;

// Override base_config table with cell parameter values using dotted paths.
[[nodiscard]] Result<toml::table> override_config_with_cell_params(
    const toml::table& base_config,
    const SweepCell& cell);

// Create run directory at sweep_root/cell_<id>/ and return its path.
[[nodiscard]] Result<fs::path> create_cell_directory(
    const fs::path& sweep_root,
    int cell_id);

// Parse gm-backtest backtest_results.json and extract sharpe_ratio_daily.
[[nodiscard]] Result<double> extract_sharpe_from_backtest_results(
    const fs::path& backtest_results_path);

// Symlink upstream artifacts (gm-universe through gm-boundaries) into cell directory.
[[nodiscard]] Result<void> link_upstream_artifacts(
    const fs::path& cell_dir,
    const fs::path& upstream_root);

} // namespace gm::sweep::execution
